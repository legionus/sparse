/*
 * Example of how to write a compiler with sparse
 */
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "symbol.h"
#include "expression.h"
#include "linearize.h"
#include "flow.h"
#include "storage.h"

struct hardreg {
	const char *name;
	pseudo_t contains;
	unsigned busy:1,
		 dirty:1,
		 used:1;
};

static void output_bb(struct basic_block *bb, unsigned long generation);

/*
 * We only know about the caller-clobbered registers
 * right now.
 */
static struct hardreg hardregs[] = {
	{ .name = "eax" },
	{ .name = "edx" },
	{ .name = "ecx" },
};
#define REGNO (sizeof(hardregs)/sizeof(struct hardreg))

struct bb_state {
	int last_reg;
	struct basic_block *bb;
	unsigned long stack_offset;
	struct storage_hash_list *inputs;
	struct storage_hash_list *outputs;
	struct storage_hash_list *internal;
};

static struct storage_hash *find_storage_hash(pseudo_t pseudo, struct storage_hash_list *list)
{
	struct storage_hash *entry;
	FOR_EACH_PTR(list, entry) {
		if (entry->pseudo == pseudo)
			return entry;
	} END_FOR_EACH_PTR(entry);
	return NULL;
}

static struct storage_hash *find_or_create_hash(pseudo_t pseudo, struct storage_hash_list **listp)
{
	struct storage_hash *entry;

	entry = find_storage_hash(pseudo, *listp);
	if (!entry) {
		entry = alloc_storage_hash(alloc_storage());
		entry->pseudo = pseudo;
		add_ptr_list(listp, entry);
	}
	return entry;
}

static const char *show_memop(struct storage *storage)
{
	static char buffer[1000];
	switch (storage->type) {
	case REG_ARG:
		sprintf(buffer, "%d(FP)", storage->regno * 4);
		break;
	case REG_STACK:
		sprintf(buffer, "%d(SP)", storage->offset);
		break;
	case REG_REG:
		return hardregs[storage->regno].name;
	default:
		return show_storage(storage);
	}
	return buffer;
}

static void alloc_stack(struct bb_state *state, struct storage *storage)
{
	storage->type = REG_STACK;
	storage->offset = state->stack_offset;
	state->stack_offset += 4;
}

/* Flush a hardreg out to the storage it has.. */
static void flush_reg(struct bb_state *state, struct hardreg *hardreg)
{
	pseudo_t pseudo;
	struct storage_hash *out;
	struct storage *storage;

	if (!hardreg->busy || !hardreg->dirty)
		return;
	hardreg->busy = 0;
	hardreg->dirty = 0;
	hardreg->used = 1;
	pseudo = hardreg->contains;
	if (!pseudo)
		return;
	if (pseudo->type != PSEUDO_REG && pseudo->type != PSEUDO_ARG)
		return;

	out = find_storage_hash(pseudo, state->internal);
	if (!out) {
		out = find_storage_hash(pseudo, state->outputs);
		if (!out)
			out = find_or_create_hash(pseudo, &state->internal);
	}
	storage = out->storage;
	switch (storage->type) {
	default:
		/*
		 * Aieee - the next user wants it in a register, but we
		 * need to flush it to memory in between. Which means that
		 * we need to allocate an internal one, dammit..
		 */
		out = find_or_create_hash(pseudo, &state->internal);
		storage = out->storage;
		/* Fall through */
	case REG_UDEF:
		alloc_stack(state, storage);
		/* Fall through */
	case REG_STACK:
		printf("\tmovl %s,%s\n", hardreg->name, show_memop(storage));
		break;
	}
}

/* Fill a hardreg with the pseudo it has */
static struct hardreg *fill_reg(struct bb_state *state, struct hardreg *hardreg, pseudo_t pseudo)
{
	struct storage_hash *src;
	struct instruction *def;

	switch (pseudo->type) {
	case PSEUDO_VAL:
		printf("\tmovl $%lld,%s\n", pseudo->value, hardreg->name);
		break;
	case PSEUDO_SYM:
		printf("\tmovl $<%s>,%s\n", show_pseudo(pseudo), hardreg->name);
		break;
	case PSEUDO_ARG:
	case PSEUDO_REG:
		def = pseudo->def;
		if (def->opcode == OP_SETVAL) {
			printf("\tmovl $<%s>,%s\n", show_pseudo(def->symbol), hardreg->name);
			break;
		}
		src = find_storage_hash(pseudo, state->internal);
		if (!src) {
			src = find_storage_hash(pseudo, state->inputs);
			if (!src) {
				src = find_storage_hash(pseudo, state->outputs);
				/* Undefined? Screw it! */
				if (!src) {
					printf("\tundef %s ??\n", show_pseudo(pseudo));
					break;
				}
			}
		}
		if (src->storage->type == REG_UDEF) {
			if (!hardreg->used) {
				src->storage->type = REG_REG;
				src->storage->regno = hardreg - hardregs;
				break;
			}
			alloc_stack(state, src->storage);
		}
		printf("\tmov.%d %s,%s\n", 32, show_memop(src->storage), hardreg->name);
		break;
	default:
		printf("\treload %s from %s\n", hardreg->name, show_pseudo(pseudo));
		break;
	}
	return hardreg;
}

static struct hardreg *target_reg(struct bb_state *state, pseudo_t pseudo)
{
	int i;
	struct hardreg *hardreg;

	i = state->last_reg+1;
	if (i >= REGNO)
		i = 0;
	state->last_reg = i;
	hardreg = hardregs + i;
	flush_reg(state, hardreg);

	hardreg->contains = pseudo;
	hardreg->busy = 1;
	hardreg->dirty = 0;
	return hardreg;
}

static struct hardreg *getreg(struct bb_state *state, pseudo_t pseudo)
{
	int i;
	struct hardreg *hardreg;

	for (i = 0; i < REGNO; i++) {
		if (hardregs[i].contains == pseudo) {
			state->last_reg = i;
			return hardregs+i;
		}
	}
	hardreg = target_reg(state, pseudo);
	return fill_reg(state, hardreg, pseudo);
}

static const char *address(struct bb_state *state, struct instruction *memop)
{
	struct symbol *sym;
	struct hardreg *base;
	static char buffer[100];
	pseudo_t addr = memop->src;

	switch(addr->type) {
	case PSEUDO_SYM:
		sym = addr->sym;
		if (sym->ctype.modifiers & MOD_NONLOCAL) {
			sprintf(buffer, "%s+%d", show_ident(sym->ident), memop->offset);
			return buffer;
		}
		sprintf(buffer, "%d+%s(SP)", memop->offset, show_ident(sym->ident));
		return buffer;
	default:
		base = getreg(state, addr);
		sprintf(buffer, "%d(%s)", memop->offset, base->name);
		return buffer;
	}
}

static const char *reg_or_imm(struct bb_state *state, pseudo_t pseudo)
{
	switch(pseudo->type) {
	case PSEUDO_VAL:
		return show_pseudo(pseudo);
	default:
		return getreg(state, pseudo)->name;
	}
}

static void generate_store(struct instruction *insn, struct bb_state *state)
{
	printf("\tmov.%d %s,%s\n", insn->size, reg_or_imm(state, insn->target), address(state, insn));
}

static void generate_load(struct instruction *insn, struct bb_state *state)
{
	const char *input = address(state, insn);
	printf("\tmov.%d %s,%s\n", insn->size, input, target_reg(state, insn->target)->name);
}

static const char* opcodes[] = {
	[OP_BADOP] = "bad_op",

	/* Fn entrypoint */
	[OP_ENTRY] = "<entry-point>",

	/* Terminator */
	[OP_RET] = "ret",
	[OP_BR] = "br",
	[OP_SWITCH] = "switch",
	[OP_INVOKE] = "invoke",
	[OP_COMPUTEDGOTO] = "jmp *",
	[OP_UNWIND] = "unwind",
	
	/* Binary */
	[OP_ADD] = "add",
	[OP_SUB] = "sub",
	[OP_MUL] = "mul",
	[OP_DIV] = "div",
	[OP_MOD] = "mod",
	[OP_SHL] = "shl",
	[OP_SHR] = "shr",
	
	/* Logical */
	[OP_AND] = "and",
	[OP_OR] = "or",
	[OP_XOR] = "xor",
	[OP_AND_BOOL] = "and-bool",
	[OP_OR_BOOL] = "or-bool",

	/* Binary comparison */
	[OP_SET_EQ] = "seteq",
	[OP_SET_NE] = "setne",
	[OP_SET_LE] = "setle",
	[OP_SET_GE] = "setge",
	[OP_SET_LT] = "setlt",
	[OP_SET_GT] = "setgt",
	[OP_SET_B] = "setb",
	[OP_SET_A] = "seta",
	[OP_SET_BE] = "setbe",
	[OP_SET_AE] = "setae",

	/* Uni */
	[OP_NOT] = "not",
	[OP_NEG] = "neg",

	/* Special three-input */
	[OP_SEL] = "select",
	
	/* Memory */
	[OP_MALLOC] = "malloc",
	[OP_FREE] = "free",
	[OP_ALLOCA] = "alloca",
	[OP_LOAD] = "load",
	[OP_STORE] = "store",
	[OP_SETVAL] = "set",
	[OP_GET_ELEMENT_PTR] = "getelem",

	/* Other */
	[OP_PHI] = "phi",
	[OP_PHISOURCE] = "phisrc",
	[OP_CAST] = "cast",
	[OP_PTRCAST] = "ptrcast",
	[OP_CALL] = "call",
	[OP_VANEXT] = "va_next",
	[OP_VAARG] = "va_arg",
	[OP_SLICE] = "slice",
	[OP_SNOP] = "snop",
	[OP_LNOP] = "lnop",
	[OP_NOP] = "nop",
	[OP_DEATHNOTE] = "dead",
	[OP_ASM] = "asm",

	/* Sparse tagging (line numbers, context, whatever) */
	[OP_CONTEXT] = "context",
};

static void generate_binop(struct bb_state *state, struct instruction *insn)
{
	const char *op = opcodes[insn->opcode];
	struct hardreg *reg = getreg(state, insn->src1);
	flush_reg(state, reg);
	printf("\t%s.%d %s,%s\n", op, insn->size, reg_or_imm(state, insn->src2), reg->name);
	reg->contains = insn->target;
	reg->busy = 1;
	reg->dirty = 1;
}

static void mark_pseudo_dead(pseudo_t pseudo)
{
	int i;

	for (i = 0; i < REGNO; i++) {
		if (hardregs[i].contains == pseudo) {
			hardregs[i].busy = 0;
			hardregs[i].dirty = 0;
		}
	}
}

static void generate_phisource(struct instruction *insn, struct bb_state *state)
{
	struct instruction *user = first_instruction(insn->phi_users);
	struct hardreg *reg = getreg(state, insn->phi_src);

	if (!user)
		return;
	reg = getreg(state, insn->phi_src); 

	flush_reg(state, reg);
	reg->contains = user->target;
	reg->busy = 1;
	reg->dirty = 1;
}

static void generate_output_storage(struct bb_state *state);

static void generate_branch(struct bb_state *state, struct instruction *br)
{
	if (br->cond) {
		struct hardreg *reg = getreg(state, br->cond);
		printf("\ttestl %s,%s\n", reg->name, reg->name);
	}
	generate_output_storage(state);
	if (br->cond)
		printf("\tje .L%p\n", br->bb_false);
	printf("\tjmp .L%p\n", br->bb_true);
}

static void generate_one_insn(struct instruction *insn, struct bb_state *state)
{
	switch (insn->opcode) {
	case OP_ENTRY: {
		struct symbol *sym = insn->bb->ep->name;
		const char *name = show_ident(sym->ident);
		if (sym->ctype.modifiers & MOD_STATIC)
			printf("\n\n%s:\n", name);
		else
			printf("\n\n.globl %s\n%s:\n", name, name);
		break;
	}

	/*
	 * OP_PHI doesn't actually generate any code. It has been
	 * done by the storage allocator and the OP_PHISOURCE.
	 */
	case OP_PHI:
		break;

	case OP_PHISOURCE:
		generate_phisource(insn, state);
		break;

	/*
	 * OP_SETVAL likewise doesn't actually generate any
	 * code. On use, the "def" of the pseudo will be
	 * looked up.
	 */
	case OP_SETVAL:
		break;

	case OP_STORE:
		generate_store(insn, state);
		break;

	case OP_LOAD:
		generate_load(insn, state);
		break;

	case OP_DEATHNOTE:
		mark_pseudo_dead(insn->target);
		break;

	case OP_BINARY ... OP_BINARY_END:
	case OP_BINCMP ... OP_BINCMP_END:
		generate_binop(state, insn);
		break;

	case OP_BR:
		generate_branch(state, insn);
		break;

	default:
		show_instruction(insn);
		break;
	}
}

static void write_reg_to_storage(struct bb_state *state, struct hardreg *reg, struct storage *storage)
{
	struct hardreg *out;

	switch (storage->type) {
	case REG_UDEF:
		storage->type = REG_REG;
		storage->regno = reg - hardregs;
		return;
	case REG_REG:
		out = hardregs + storage->regno;
		if (reg == out)
			return;
		assert(!out->contains);
		printf("\tmovl %s,%s\n", reg->name, out->name);
		return;
	default:
		printf("\tmovl %s,%s\n", reg->name, show_memop(storage));
		return;
	}
}

static void write_val_to_storage(struct bb_state *state, pseudo_t src, struct storage *storage)
{
	struct hardreg *out;

	switch (storage->type) {
	case REG_UDEF:
		alloc_stack(state, storage);
	default:
		printf("\tmovl %s,%s\n", show_pseudo(src), show_memop(storage));
		break;
	case REG_REG:
		out = hardregs + storage->regno;
		printf("\tmovl %s,%s\n", show_pseudo(src), out->name);
	}
}

static void fill_output(struct bb_state *state, pseudo_t pseudo, struct storage *out)
{
	int i;
	struct storage_hash *in;
	struct instruction *def;

	/* Is that pseudo a constant value? */
	switch (pseudo->type) {
	case PSEUDO_VAL:
		write_val_to_storage(state, pseudo, out);
		return;
	case PSEUDO_REG:
		def = pseudo->def;
		if (def->opcode == OP_SETVAL) {
			write_val_to_storage(state, def->symbol, out);
			return;
		}
	default:
		break;
	}

	/* See if we have that pseudo in a register.. */
	for (i = 0; i < REGNO; i++) {
		struct hardreg *reg = hardregs + i;
		if (reg->contains != pseudo)
			continue;
		write_reg_to_storage(state, reg, out);
		return;
	}

	/* Do we have it in another storage? */
	in = find_storage_hash(pseudo, state->internal);
	if (!in) {
		in = find_storage_hash(pseudo, state->inputs);
		/* Undefined? */
		if (!in)
			return;
	}
	switch (out->type) {
	case REG_UDEF:
		*out = *in->storage;
		break;
	case REG_REG:
		printf("\tmovl %s,%s\n", show_memop(in->storage), hardregs[out->regno].name);
		break;
	default:
		printf("\tmovl %s,tmp\n", show_memop(in->storage));
		printf("\tmovl tmp,%s\n", show_memop(out));
		break;
	}
	return;
}

/*
 * This tries to make sure that we put all the pseudos that are
 * live on exit into the proper storage
 */
static void generate_output_storage(struct bb_state *state)
{
	struct storage_hash *entry;

	/* Go through the fixed outputs, making sure we have those regs free */
	FOR_EACH_PTR(state->outputs, entry) {
		struct storage *out = entry->storage;
		if (out->type == REG_REG) {
			struct hardreg *reg = hardregs + out->regno;
			if (reg->contains != entry->pseudo) {
				flush_reg(state, reg);
				reg->contains = NULL;
			}
		}
	} END_FOR_EACH_PTR(entry);

	FOR_EACH_PTR(state->outputs, entry) {
		fill_output(state, entry->pseudo, entry->storage);
	} END_FOR_EACH_PTR(entry);
}

static void generate(struct basic_block *bb, struct bb_state *state)
{
	int i;
	struct storage_hash *entry;
	struct instruction *insn;

	for (i = 0; i < REGNO; i++) {
		hardregs[i].contains = NULL;
		hardregs[i].busy = 0;
		hardregs[i].dirty = 0;
		hardregs[i].used = 0;
	}

	FOR_EACH_PTR(state->inputs, entry) {
		struct storage *storage = entry->storage;
		const char *name = show_storage(storage);
		if (storage->type == REG_REG) {
			int regno = storage->regno;
			hardregs[regno].contains = entry->pseudo;
			hardregs[regno].busy = 1;
			hardregs[regno].dirty = 1;
			name = hardregs[regno].name;
		}
	} END_FOR_EACH_PTR(entry);

	printf(".L%p:\n", bb);
	FOR_EACH_PTR(bb->insns, insn) {
		if (!insn->bb)
			continue;
		generate_one_insn(insn, state);
	} END_FOR_EACH_PTR(insn);

	if (verbose) {
		printf("\t---\n");
		FOR_EACH_PTR(state->inputs, entry) {
			printf("\t%s <- %s\n", show_pseudo(entry->pseudo), show_storage(entry->storage));
		} END_FOR_EACH_PTR(entry);
		printf("\t---\n");
		FOR_EACH_PTR(state->internal, entry) {
			printf("\t%s <-> %s\n", show_pseudo(entry->pseudo), show_storage(entry->storage));
		} END_FOR_EACH_PTR(entry);
		printf("\t---\n");
		FOR_EACH_PTR(state->outputs, entry) {
			printf("\t%s -> %s\n", show_pseudo(entry->pseudo), show_storage(entry->storage));
		} END_FOR_EACH_PTR(entry);
	}
	printf("\n");
}

static void generate_list(struct basic_block_list *list, unsigned long generation)
{
	struct basic_block *bb;
	FOR_EACH_PTR(list, bb) {
		if (bb->generation == generation)
			continue;
		output_bb(bb, generation);
	} END_FOR_EACH_PTR(bb);
}

static void output_bb(struct basic_block *bb, unsigned long generation)
{
	struct bb_state state;

	bb->generation = generation;

	/* Make sure all parents have been generated first */
	generate_list(bb->parents, generation);

	state.inputs = gather_storage(bb, STOR_IN);
	state.outputs = gather_storage(bb, STOR_OUT);
	state.internal = NULL;
	state.stack_offset = 0;
	state.last_reg = -1;

	generate(bb, &state);

	free_ptr_list(&state.inputs);
	free_ptr_list(&state.outputs);

	/* Generate all children... */
	generate_list(bb->children, generation);
}

static void output(struct entrypoint *ep)
{
	unsigned long generation = ++bb_generation;

	/* Set up initial inter-bb storage links */
	set_up_storage(ep);

	/* Show the results ... */
	output_bb(ep->entry->bb, generation);

	/* Clear the storage hashes for the next function.. */
	free_storage();
}

static int compile(struct symbol_list *list)
{
	struct symbol *sym;
	FOR_EACH_PTR(list, sym) {
		struct entrypoint *ep;
		expand_symbol(sym);
		ep = linearize_symbol(sym);
		if (ep)
			output(ep);
	} END_FOR_EACH_PTR(sym);
	
	return 0;
}

int main(int argc, char **argv)
{
	return compile(sparse(argc, argv));
}
