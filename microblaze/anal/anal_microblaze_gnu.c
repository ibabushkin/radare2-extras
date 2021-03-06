/* GPL, Copyright 2015 - tic */

#include <string.h>
#include <r_types.h>
#include <r_lib.h>
#include <r_asm.h>
#include <r_anal.h>

#include "microblaze-opc.h"
#include "dis-asm.h"

#define get_int_field_imm(instr)   ((instr & IMM_MASK) >> IMM_LOW)
#define get_int_field_r1(instr)    ((instr & RA_MASK) >> RA_LOW)
#define get_int_field_r2(instr)    ((instr & RB_MASK) >> RB_LOW)

#define get_field_rd(instr)        get_field (instr, RD_MASK, RD_LOW)
#define get_field_r1(instr)        get_field (instr, RA_MASK, RA_LOW)
#define get_field_r2(instr)        get_field (instr, RB_MASK, RB_LOW)
#define get_field_imm(instr)       get_imm   (ctx, instr)

struct mb_anal_ctx {
	int immval;
	bool immfound;
	bool immfound_addr;
	RAnal *anal;
	RAnalOp *op;
};

static char *get_field (long instr, long mask, unsigned short low) {
	char tmpstr[25];
	sprintf (tmpstr, "%s%d", register_prefix, (int)((instr & mask) >> low));
	return strdup (tmpstr);
}




static unsigned char bytes[4];
static int microblaze_read_memory (bfd_vma memaddr, bfd_byte *myaddr, unsigned int length, struct disassemble_info *info) {
	memcpy (myaddr, bytes, length);
	return 0;
}

unsigned long
microblaze_our_get_target_address (long inst, bool immfound, int immval,
				   long pcval, long r1val, long r2val,
				   bool *targetvalid,
				   bool *unconditionalbranch)
{
	struct op_code_struct * op;
	unsigned long targetaddr = 0;

	*unconditionalbranch = false;
	/* Just a linear search of the table.  */
	for (op = opcodes; op->name != 0; op ++)
		if (op->bit_sequence == (inst & op->opcode_mask))
			break;

	if (op->name == 0)
		*targetvalid = false;
	else if (op->instr_type == branch_inst) {
		switch (op->inst_type) {
		case INST_TYPE_R2:
			*unconditionalbranch = true;
		/* Fall through.  */
		case INST_TYPE_RD_R2:
		case INST_TYPE_R1_R2:
			targetaddr = r2val;
			*targetvalid = true;
			if (op->inst_offset_type == INST_PC_OFFSET)
				targetaddr += pcval;
			break;
		case INST_TYPE_IMM:
			*unconditionalbranch = true;
		/* Fall through.  */
		case INST_TYPE_RD_IMM:
		case INST_TYPE_R1_IMM:
			if (immfound) {
				targetaddr = (immval << 16) & 0xffff0000;
				targetaddr |= (get_int_field_imm (inst) & 0x0000ffff);
			} else {
				targetaddr = get_int_field_imm (inst);
				if (targetaddr & 0x8000)
					targetaddr |= 0xFFFF0000;
			}
			if (op->inst_offset_type == INST_PC_OFFSET)
				targetaddr += pcval;
			*targetvalid = true;
			break;
		default:
			*targetvalid = false;
			break;
		}
	} else if (op->instr_type == return_inst) {
		if (immfound) {
			targetaddr = (immval << 16) & 0xffff0000;
			targetaddr |= (get_int_field_imm (inst) & 0x0000ffff);
		}
		else {
			targetaddr = get_int_field_imm (inst);
			if (targetaddr & 0x8000)
				targetaddr |= 0xFFFF0000;
		}
		targetaddr += r1val;
		*targetvalid = true;
	}
	else
		*targetvalid = false;
	return targetaddr;
}

static unsigned long
read_insn_microblaze (bfd_vma memaddr,
		      struct disassemble_info *info,
		      struct op_code_struct **opr)
{
  unsigned char       ibytes[4];
  int                 status;
  struct op_code_struct * op;
  unsigned long inst;

  status = info->read_memory_func (memaddr, ibytes, 4, info);

  if (status != 0) return 0;

  if (info->endian == BFD_ENDIAN_BIG)
    inst = (ibytes[0] << 24) | (ibytes[1] << 16) | (ibytes[2] << 8) | ibytes[3];
  else if (info->endian == BFD_ENDIAN_LITTLE)
    inst = (ibytes[3] << 24) | (ibytes[2] << 16) | (ibytes[1] << 8) | ibytes[0];
  else
    abort ();

  /* Just a linear search of the table.  */
  for (op = opcodes; op->name != 0; op ++)
    if (op->bit_sequence == (inst & op->opcode_mask))
      break;

  *opr = op;
  return inst;
}

static char *get_imm (struct mb_anal_ctx *ctx, int instr) {
	char tmpstr[25];
	int immval = 0;
	if (ctx->immfound)
		immval = ctx->immval << 16 & 0xFFFF0000;
	immval |= get_int_field_imm(instr);
	sprintf (tmpstr, "%d", immval);
	return strdup(tmpstr);
}

static void handle_immediate_inst(struct mb_anal_ctx *ctx, unsigned long insn, struct op_code_struct *mb_op) {
	if (mb_op->instr == imm) {
		ctx->immval = get_int_field_imm (insn);
		ctx->immfound_addr = ctx->op->addr;
	}
}

static void analyse_arithmetic_inst(struct mb_anal_ctx *ctx, unsigned long insn, struct op_code_struct *mb_op) {
	RAnalOp *op = ctx->op;
	char *ra = get_field_r1(insn);
	char *rb = get_field_r2(insn);
	char *rd = get_field_rd(insn);
	char *imm = get_imm(ctx, insn);
	switch (mb_op->instr) {
	case add:
		r_strbuf_setf(&op->esil, "%s,%s,+,%s,=",ra, rb, rd);
		op->type = R_ANAL_OP_TYPE_ADD;
		break;
	case rsub:
		r_strbuf_setf(&op->esil, "%s,%s,-,%s,=",ra, rb, rd);
		op->type = R_ANAL_OP_TYPE_SUB;
		break;
	case addc:
		op->type = R_ANAL_OP_TYPE_ADD;
		break;
	case rsubc:
		op->type = R_ANAL_OP_TYPE_SUB;
		break;
	case addk:
		/* should keep carry */
		r_strbuf_setf(&op->esil, "%s,%s,+,%s,=",ra, rb, rd);
		op->type = R_ANAL_OP_TYPE_ADD;
		break;
	case rsubk:
		/* should keep carry */
		r_strbuf_setf(&op->esil, "%s,%s,-,%s,=",ra, rb, rd);
		op->type = R_ANAL_OP_TYPE_SUB;
		break;
	case cmp:
		op->type = R_ANAL_OP_TYPE_CMP;
		break;
	case cmpu:
		op->type = R_ANAL_OP_TYPE_CMP;
		break;
	case addkc:
		op->type = R_ANAL_OP_TYPE_ADD;
		break;
	case rsubkc:
		op->type = R_ANAL_OP_TYPE_SUB;
		break;
	case addi:
		r_strbuf_setf(&op->esil, "%s,%s,+,%s,=", ra, imm, rd);
		op->type = R_ANAL_OP_TYPE_ADD;
		break;
	case rsubi:
		r_strbuf_setf(&op->esil, "%s,%s,-,%s,=", ra, imm, rd);
		op->type = R_ANAL_OP_TYPE_SUB;
		break;
	case addic:
		/* should use carry */
		r_strbuf_setf(&op->esil, "%s,%s,+,%s,=", ra, imm, rd);
		op->type = R_ANAL_OP_TYPE_ADD;
		break;
	case rsubic:
		/* should use carry */
		r_strbuf_setf(&op->esil, "%s,%s,-,%s,=", ra, imm, rd);
		op->type = R_ANAL_OP_TYPE_SUB;
		break;
	case addik:
		/* should keep carry */
		r_strbuf_setf(&op->esil, "%s,%s,+,%s,=", ra, imm, rd);
		op->type = R_ANAL_OP_TYPE_ADD;
		break;
	case rsubik:
		/* should keep carry */
		r_strbuf_setf(&op->esil, "%s,%s,-,%s,=", ra, imm, rd);
		op->type = R_ANAL_OP_TYPE_SUB;
		break;
	case addikc:
		/* should use and keep carry */
		r_strbuf_setf(&op->esil, "%s,%s,+,%s,=", ra, imm, rd);
		op->type = R_ANAL_OP_TYPE_ADD;
		break;
	case rsubikc:
		/* should use and keep carry */
		r_strbuf_setf(&op->esil, "%s,%s,-,%s,=", ra, imm, rd);
		op->type = R_ANAL_OP_TYPE_SUB;
		break;
	case swapb:
		break;
	case swaph:
		break;
	default:
		break;
	}
}

static void analyse_logical_inst(struct mb_anal_ctx *ctx, unsigned long insn, struct op_code_struct *mb_op) {
	RAnalOp *op = ctx->op;
	char *ra = get_field_r1(insn);
	char *rb = get_field_r2(insn);
	char *rd = get_field_rd(insn);
	char *imm = get_imm(ctx, insn);
	switch (mb_op->instr) {
	case or:
		r_strbuf_setf(&op->esil, "%s,%s,|,%s,=", ra, rb, rd);
		op->type = R_ANAL_OP_TYPE_OR;
		break;
	case and:
		r_strbuf_setf(&op->esil, "%s,%s,&,%s,=", ra, rb, rd);
		op->type = R_ANAL_OP_TYPE_AND;
		break;
	case xor:
		r_strbuf_setf(&op->esil, "%s,%s,^,%s,=", ra, rb, rd);
		op->type = R_ANAL_OP_TYPE_XOR;
		break;
	case andn:
		/* nand */
		r_strbuf_setf(&op->esil, "%s,%s,!,&,%s,=", ra, rb, rd);
		op->type = R_ANAL_OP_TYPE_NOT;
		break;
	case pcmpbf:
		break;
	case pcmpbc:
		break;
	case pcmpeq:
		break;
	case pcmpne:
		break;
	case sra:
		break;
	case src:
		break;
	case srl:
		break;
	case sext8:
		break;
	case sext16:
		break;
	case ori:
		r_strbuf_setf(&op->esil, "%s,%s,|,%s,=", ra, imm, rd);
		op->type = R_ANAL_OP_TYPE_OR;
		break;
	case andi:
		r_strbuf_setf(&op->esil, "%s,%s,&,%s,=", ra, imm, rd);
		op->type = R_ANAL_OP_TYPE_AND;
		break;
	case xori:
		r_strbuf_setf(&op->esil, "%s,%s,^,%s,=", ra, imm, rd);
		op->type = R_ANAL_OP_TYPE_XOR;
		break;
	case andni:
		r_strbuf_setf(&op->esil, "%s,!,%s,&,%s,=", ra, imm, rd);
		op->type = R_ANAL_OP_TYPE_NOT;
		break;
	default:
		break;
	}
}

static void analyse_mult_inst(struct mb_anal_ctx *ctx, unsigned long insn, struct op_code_struct *mb_op) {
	RAnalOp *op = ctx->op;
	char *ra = get_field_r1(insn);
	char *rb = get_field_r2(insn);
	char *rd = get_field_rd(insn);
	char *imm = get_imm(ctx, insn);
	switch (mb_op->instr) {
	case mul:
		/* should get the LSW of the mul */
		r_strbuf_setf(&op->esil, "%s,%s,*,%s,=", ra, rb, rd);
		op->type = R_ANAL_OP_TYPE_MUL;
		break;
	case mulh:
		/* should get the signed MSW of the mul */
		op->type = R_ANAL_OP_TYPE_MUL;
		break;
	case mulhu:
		/* should get the unsigned MSW of the mul */
		op->type = R_ANAL_OP_TYPE_MUL;
		break;
	case mulhsu:
		/* should get the signed MSW of the mul signed * unsigned */
		op->type = R_ANAL_OP_TYPE_MUL;
		break;
	case muli:
		/* should get the LSW of the mul */
		r_strbuf_setf(&op->esil, "%s,%s,*,%s,=", ra, imm, rd);
		op->type = R_ANAL_OP_TYPE_MUL;
	default:
		break;
	}
}

static void analyse_div_inst(struct mb_anal_ctx *ctx, unsigned long insn, struct op_code_struct *mb_op) {
	RAnalOp *op = ctx->op;
	char *ra = get_field_r1(insn);
	char *rb = get_field_r2(insn);
	char *rd = get_field_rd(insn);

	r_strbuf_setf(&op->esil, "");
	switch (mb_op->instr) {
	case idiv:
		r_strbuf_setf(&op->esil, "%s,%s,/,%s,=", ra, rb, rd);
		op->type = R_ANAL_OP_TYPE_DIV;
		break;
	case idivu:
		r_strbuf_setf(&op->esil, "%s,%s,/,%s,=", ra, rb, rd);
		op->type = R_ANAL_OP_TYPE_DIV;
		break;
	default:
		break;
	}
}

static char *long_to_string(long imm) {
	char tmpstr[25];
	sprintf(tmpstr, "%"PFMT64d, (ut64)imm);
	return strdup(tmpstr);
}

static void analyse_branch_inst(struct mb_anal_ctx *ctx, unsigned long insn, struct op_code_struct *mb_op) {
	RAnalOp *op = ctx->op;
	bool targetvalid;
	bool unconditionalbranch;
	long r1 = get_int_field_r1(insn);
	long r2 = get_int_field_r2(insn);
	char *ra = get_field_r1(insn);
	char *rb = get_field_r2(insn);
	char *rd = get_field_rd(insn);
	char *imm;
	unsigned long jump_addr = 0;
	r_strbuf_setf(&op->esil, "");


	jump_addr = microblaze_our_get_target_address (insn,
			ctx->immfound, ctx->immval,
			ctx->op->addr, r1, r2,
			&targetvalid,
			&unconditionalbranch);

	// hack until I figure where the bug is in this gnu fcn
	if (!ctx->immfound)
		jump_addr &= 0xFFFF;

	imm = long_to_string(jump_addr);

	switch (mb_op->instr) {
	case br:
		r_strbuf_setf(&op->esil, "%s,pc,=+", rb);
		op->type = R_ANAL_OP_TYPE_UJMP;
		break;
	case brd:
		r_strbuf_setf(&op->esil, "%s,pc,=+", rb);
		op->type = R_ANAL_OP_TYPE_UJMP;
		break;
	case brld:
		r_strbuf_setf(&op->esil, "%s,pc,=+,pc,%s,=,", rb, rd);
		op->type = R_ANAL_OP_TYPE_UJMP;
		op->delay = 1;
		break;
	case bra:
		r_strbuf_setf(&op->esil, "%s,pc,=", rb);
		op->type = R_ANAL_OP_TYPE_UJMP;
		break;
	case brad:
		r_strbuf_setf(&op->esil, "%s,pc,=", rb);
		op->type = R_ANAL_OP_TYPE_UJMP;
		op->delay = 1;
		break;
	case brald:
		r_strbuf_setf(&op->esil, "%s,pc,=,pc,%s,=,", rb, rd);
		op->type = R_ANAL_OP_TYPE_UJMP;
		op->delay = 1;
		break;
	case microblaze_brk:
		r_strbuf_setf(&op->esil, "%s,pc,=,pc,%s,=,TRAP", rb, rd);
		op->type = R_ANAL_OP_TYPE_UJMP;
		break;
	case beq:
		r_strbuf_setf(&op->esil, "%s,0,==,?{,%s,pc,=,}", ra, rb);
		op->type = R_ANAL_OP_TYPE_UCJMP;
		break;
	case beqd:
		r_strbuf_setf(&op->esil, "%s,0,==,?{,%s,pc,=,}", ra, rb);
		op->type = R_ANAL_OP_TYPE_UCJMP;
		op->delay = 1;
		break;
	case bne:
		r_strbuf_setf(&op->esil, "%s,0,==,!,?{,%s,pc,=,}", ra, rb);
		op->type = R_ANAL_OP_TYPE_UCJMP;
		break;
	case bned:
		r_strbuf_setf(&op->esil, "%s,0,==,!,?{,%s,pc,=,}", ra, rb);
		op->type = R_ANAL_OP_TYPE_UCJMP;
		op->delay = 1;
		break;
	case blt:
		r_strbuf_setf(&op->esil, "0,%s,<,?{,%s,pc,=,}", ra, rb);
		op->type = R_ANAL_OP_TYPE_UCJMP;
		break;
	case bltd:
		r_strbuf_setf(&op->esil, "0,%s,<,?{,%s,pc,=,}", ra, rb);
		op->type = R_ANAL_OP_TYPE_UCJMP;
		op->delay = 1;
		break;
	case ble:
		r_strbuf_setf(&op->esil, "0,%s,<=,?{,%s,pc,=,}", ra, rb);
		op->type = R_ANAL_OP_TYPE_UCJMP;
		break;
	case bgt:
		r_strbuf_setf(&op->esil, "0,%s,>,?{,%s,pc,=,}", ra, rb);
		op->type = R_ANAL_OP_TYPE_UCJMP;
		break;
	case bgtd:
		r_strbuf_setf(&op->esil, "0,%s,>,?{,%s,pc,=,}", ra, rb);
		op->type = R_ANAL_OP_TYPE_UCJMP;
		op->delay = 1;
		break;
	case bge:
		r_strbuf_setf(&op->esil, "0,%s,>=,?{,%s,pc,=,}", ra, rb);
		op->type = R_ANAL_OP_TYPE_UCJMP;
		break;
	case bged:
		r_strbuf_setf(&op->esil, "0,%s,>=,?{,%s,pc,=,}", ra, rb);
		op->type = R_ANAL_OP_TYPE_UCJMP;
		op->delay = 1;
		break;
	case bri:
		r_strbuf_setf(&op->esil, "%s,pc,=", imm);
		op->type = R_ANAL_OP_TYPE_JMP;
		op->jump = jump_addr;
		break;
	case brid:
		r_strbuf_setf(&op->esil, "%s,pc,=", imm);
		op->type = R_ANAL_OP_TYPE_JMP;
		op->delay = 1;
		op->jump = jump_addr;
		break;
	case brlid:
		r_strbuf_setf(&op->esil, "%s,pc,=", imm);
		op->type = R_ANAL_OP_TYPE_JMP;
		op->delay = 1;
		op->jump = jump_addr;
		break;
	case brai:
		r_strbuf_setf(&op->esil, "%s,pc,=", imm);
		op->type = R_ANAL_OP_TYPE_JMP;
		op->jump = jump_addr;
		break;
	case braid:
		r_strbuf_setf(&op->esil, "%s,pc,=", imm);
		op->type = R_ANAL_OP_TYPE_JMP;
		op->delay = 1;
		op->jump = jump_addr;
		break;
	case bralid:
		r_strbuf_setf(&op->esil, "%s,pc,=", imm);
		op->type = R_ANAL_OP_TYPE_JMP;
		op->delay = 1;
		op->jump = jump_addr;
		break;
	case brki:
		r_strbuf_setf(&op->esil, "TRAP", imm);
		op->type = R_ANAL_OP_TYPE_JMP;
		op->jump = jump_addr;
		break;
	case beqi:
		r_strbuf_setf(&op->esil, "0,%s,==,?{,%s,pc,=,}", ra, imm);
		op->type = R_ANAL_OP_TYPE_CJMP;
		op->jump = jump_addr;
		op->fail = op->addr + op->size;
		break;
	case beqid:
		r_strbuf_setf(&op->esil, "0,%s,==,?{,%s,pc,=,}", ra, imm);
		op->type = R_ANAL_OP_TYPE_CJMP;
		op->delay = 1;
		op->jump = jump_addr;
		op->fail = op->addr + op->size;
		break;
	case bnei:
		r_strbuf_setf(&op->esil, "0,%s,==,!,?{,%s,pc,=,}", ra, imm);
		op->type = R_ANAL_OP_TYPE_CJMP;
		op->jump = jump_addr;
		op->fail = op->addr + op->size;
		break;
	case bneid:
		r_strbuf_setf(&op->esil, "0,%s,==,!,?{,%s,pc,=,}", ra, imm);
		op->type = R_ANAL_OP_TYPE_CJMP;
		op->delay = 1;
		op->jump = jump_addr;
		op->fail = op->addr + op->size;
		break;
	case blti:
		r_strbuf_setf(&op->esil, "0,%s,<,?{,%s,pc,=,}", ra, imm);
		op->type = R_ANAL_OP_TYPE_CJMP;
		op->jump = jump_addr;
		op->fail = op->addr + op->size;
		break;
	case bltid:
		r_strbuf_setf(&op->esil, "0,%s,<,?{,%s,pc,=,}", ra, imm);
		op->type = R_ANAL_OP_TYPE_CJMP;
		op->delay = 1;
		op->jump = jump_addr;
		op->fail = op->addr + op->size;
		break;
	case blei:
		r_strbuf_setf(&op->esil, "0,%s,<=,?{,%s,pc,=,}", ra, imm);
		op->type = R_ANAL_OP_TYPE_CJMP;
		op->jump = jump_addr;
		op->fail = op->addr + op->size;
		break;
	case bleid:
		r_strbuf_setf(&op->esil, "0,%s,<=,?{,%s,pc,=,}", ra, imm);
		op->type = R_ANAL_OP_TYPE_CJMP;
		op->delay = 1;
		op->jump = jump_addr;
		op->fail = op->addr + op->size;
		break;
	case bgti:
		r_strbuf_setf(&op->esil, "0,%s,>,?{,%s,pc,=,}", ra, imm);
		op->type = R_ANAL_OP_TYPE_CJMP;
		op->jump = jump_addr;
		op->fail = op->addr + op->size;
		break;
	case bgtid:
		r_strbuf_setf(&op->esil, "0,%s,>,?{,%s,pc,=,}", ra, imm);
		op->type = R_ANAL_OP_TYPE_CJMP;
		op->delay = 1;
		op->jump = jump_addr;
		op->fail = op->addr + op->size;
		break;
	case bgei:
		r_strbuf_setf(&op->esil, "0,%s,>=,?{,%s,pc,=,}", ra, imm);
		op->type = R_ANAL_OP_TYPE_CJMP;
		op->jump = jump_addr;
		op->fail = op->addr + op->size;
		break;
	case bgeid:
		r_strbuf_setf(&op->esil, "0,%s,>=,?{,%s,pc,=,}", ra, imm);
		op->type = R_ANAL_OP_TYPE_CJMP;
		op->delay = 1;
		op->jump = jump_addr;
		op->fail = op->addr + op->size;
		break;
	default:
		break;
	}
}

static void analyse_return_inst(struct mb_anal_ctx *ctx, unsigned long insn, struct op_code_struct *mb_op) {
	RAnalOp *op = ctx->op;
	long r1 = get_int_field_r1(insn);
	long r2 = get_int_field_r2(insn);
	char *ra = get_field_r1(insn);
	char *imm;
	bool targetvalid, unconditionalbranch;
	long jump_addr = microblaze_our_get_target_address (insn,
			ctx->immfound, ctx->immval,
			ctx->op->addr, r1, r2,
			&targetvalid,
			&unconditionalbranch);

	// hack until I figure where the bug is in this gnu fcn
	if (!ctx->immfound)
		jump_addr &= 0xFFFF;

	imm = long_to_string(jump_addr);

	switch (mb_op->instr) {
	case rtsd:
		r_strbuf_setf(&op->esil, "%s,+,%s,pc,=", ra, imm);
		op->type = R_ANAL_OP_TYPE_RET;
		op->delay = 1;
		break;
	case rtid:
		r_strbuf_setf(&op->esil, "%s,+,%s,pc,=", ra, imm);
		op->type = R_ANAL_OP_TYPE_RET;
		op->delay = 1;
		break;
	case rtbd:
		r_strbuf_setf(&op->esil, "%s,+,%s,pc,=", ra, imm);
		op->type = R_ANAL_OP_TYPE_RET;
		op->delay = 1;
		break;
	case rted:
		r_strbuf_setf(&op->esil, "%s,+,%s,pc,=", ra, imm);
		op->type = R_ANAL_OP_TYPE_RET;
		op->delay = 1;
		break;
	default:
		break;
	}
}

static void analyse_special_inst(struct mb_anal_ctx *ctx, unsigned long insn, struct op_code_struct *mb_op) {
	RAnalOp *op = ctx->op;
	switch (mb_op->instr) {
	case wic:
		break;
	case wdc:
		break;
	case wdcclear:
		break;
	case wdcextclear:
		break;
	case wdcflush:
		break;
	case wdcextflush:
		break;
	case mts:
		break;
	case mfs:
		break;
	default:
		break;
	}
}
static void analyse_anyware_inst(struct mb_anal_ctx *ctx, unsigned long insn, struct op_code_struct *mb_op) {
	RAnalOp *op = ctx->op;
	switch (mb_op->instr) {
	case get:
		break;
	case put:
		break;
	case nget:
		break;
	case nput:
		break;
	case cget:
		break;
	case cput:
		break;
	case ncget:
		break;
	case ncput:
		break;
	default:
		break;
	}
}

static void analyse_memory_load_inst(struct mb_anal_ctx *ctx, unsigned long insn, struct op_code_struct *mb_op) {
	RAnalOp *op = ctx->op;
	char *ra = get_field_r1(insn);
	char *rb = get_field_r2(insn);
	char *rd = get_field_rd(insn);
	char *imm = get_imm(ctx, insn);
	switch (mb_op->instr) {
	case lbu:
		r_strbuf_setf(&op->esil, "%s,%s,[],+,%s,=", ra, rb, rd);
		op->type = R_ANAL_OP_TYPE_LOAD;
		break;
	case lbur:
		r_strbuf_setf(&op->esil, "%s,%s,[],+,%s,=", ra, rb, rd);
		op->type = R_ANAL_OP_TYPE_LOAD;
		break;
	case lhu:
		r_strbuf_setf(&op->esil, "%s,%s,[],+,%s,=", ra, rb, rd);
		op->type = R_ANAL_OP_TYPE_LOAD;
		break;
	case lhur:
		r_strbuf_setf(&op->esil, "%s,%s,[],+,%s,=", ra, rb, rd);
		op->type = R_ANAL_OP_TYPE_LOAD;
		break;
	case lw:
		r_strbuf_setf(&op->esil, "%s,%s,[],+,%s,=", ra, rb, rd);
		op->type = R_ANAL_OP_TYPE_LOAD;
		break;
	case lwr:
		r_strbuf_setf(&op->esil, "%s,%s,[],+,%s,=", ra, rb, rd);
		op->type = R_ANAL_OP_TYPE_LOAD;
		break;
	case lwx:
		r_strbuf_setf(&op->esil, "%s,%s,[],+,%s,=", ra, rb, rd);
		op->type = R_ANAL_OP_TYPE_LOAD;
		break;
	case lbui:
		r_strbuf_setf(&op->esil, "%s,%s,[],+,%s,=", ra, imm, rd);
		op->type = R_ANAL_OP_TYPE_LOAD;
		break;
	case lhui:
		r_strbuf_setf(&op->esil, "%s,%s,[],+,%s,=", ra, imm, rd);
		op->type = R_ANAL_OP_TYPE_LOAD;
		break;
	case lwi:
		r_strbuf_setf(&op->esil, "%s,%s,[],+,%s,=", ra, imm, rd);
		op->type = R_ANAL_OP_TYPE_LOAD;
		break;
	default:
		break;
	}
}

static void analyse_memory_store_inst(struct mb_anal_ctx *ctx, unsigned long insn, struct op_code_struct *mb_op) {
	RAnalOp *op = ctx->op;
	char *ra = get_field_r1(insn);
	char *rb = get_field_r2(insn);
	char *rd = get_field_rd(insn);
	char *imm = get_imm(ctx, insn);
	switch (mb_op->instr) {
	case sb:
		r_strbuf_setf(&op->esil, "%s,%s,+,%s,=[]", rd, ra, rb);
		op->type = R_ANAL_OP_TYPE_STORE;
		break;
	case sbr:
		r_strbuf_setf(&op->esil, "%s,%s,+,%s,=[]", rd, ra, rb);
		op->type = R_ANAL_OP_TYPE_STORE;
		break;
	case sh:
		r_strbuf_setf(&op->esil, "%s,%s,+,%s,=[]", rd, ra, rb);
		op->type = R_ANAL_OP_TYPE_STORE;
		break;
	case shr:
		r_strbuf_setf(&op->esil, "%s,%s,+,%s,=[]", rd, ra, rb);
		op->type = R_ANAL_OP_TYPE_STORE;
		break;
	case sw:
		r_strbuf_setf(&op->esil, "%s,%s,+,%s,=[]", rd, ra, rb);
		op->type = R_ANAL_OP_TYPE_STORE;
		break;
	case swr:
		/* what's the diff ? not documented */
		r_strbuf_setf(&op->esil, "%s,%s,+,%s,=[]", rd, ra, rb);
		op->type = R_ANAL_OP_TYPE_STORE;
		break;
	case swx:
		/* should set reservation bit (semaphore) */
		r_strbuf_setf(&op->esil, "%s,%s,+,%s,=[]", rd, ra, rb);
		op->type = R_ANAL_OP_TYPE_STORE;
		break;
	case sbi:
		/* should only store lsb */
		r_strbuf_setf(&op->esil, "%s,%s,+,%s,=[]", rd, ra, imm);
		op->type = R_ANAL_OP_TYPE_STORE;
		break;
	case shi:
		/* should only store msb */
		r_strbuf_setf(&op->esil, "%s,%s,+,%s,=[]", rd, ra, imm);
		op->type = R_ANAL_OP_TYPE_STORE;
		break;
	case swi:
		r_strbuf_setf(&op->esil, "%s,%s,+,%s,=[]", rd, ra, imm);
		op->type = R_ANAL_OP_TYPE_STORE;
		break;
	default:
		break;
	}
}

static void analyse_barrel_shift_inst(struct mb_anal_ctx *ctx, unsigned long insn, struct op_code_struct *mb_op) {
	RAnalOp *op = ctx->op;
	switch (mb_op->instr) {
	case bsll:
		op->type = R_ANAL_OP_TYPE_SHL;
		break;
	case bsra:
		op->type = R_ANAL_OP_TYPE_SAR;
		break;
	case bsrl:
		op->type = R_ANAL_OP_TYPE_SHR;
		break;
	case bslli:
		op->type = R_ANAL_OP_TYPE_SHL;
		break;
	case bsrai:
		op->type = R_ANAL_OP_TYPE_SAR;
		break;
	case bsrli:
		op->type = R_ANAL_OP_TYPE_SHR;
		break;
	default:
		break;
	}
}


static int microblaze_op(RAnal *a, RAnalOp *op, ut64 addr, const ut8 *buf, int len) {
	int oplen = 4;
	static struct mb_anal_ctx ctx;
	static bool first_time = true;
	struct disassemble_info info;
	unsigned long insn;
	struct op_code_struct *mb_op;

	/* initialize immediate value, belongs to init,
	 * there are some corner cases here */
	if (len < 4)
		return -1;

	if (first_time) {
		ctx.immval = 0;
		ctx.immfound = false;
		ctx.immfound_addr = 0;
	}
	first_time = false;
	ctx.anal = a;
	ctx.op = op;

	/* update bytes */
	memcpy(bytes, buf, oplen);
	info.read_memory_func = microblaze_read_memory;
	if (a->big_endian) info.endian = BFD_ENDIAN_BIG;
	else info.endian = BFD_ENDIAN_LITTLE;

	if (op == NULL)
		return oplen;

	memset (op, 0, sizeof (RAnalOp));
	op->type = R_ANAL_OP_TYPE_NULL;
	op->jump = op->fail = -1;
	op->size = oplen;
	op->delay = 0;
	op->addr = addr;
	op->ptr = op->val = -1;
	op->refptr = 0;
	r_strbuf_init (&op->esil);

	/* get microblaze insn */
	insn = read_insn_microblaze(0, &info, &mb_op);

	if (insn == 0) {
		op->type = R_ANAL_OP_TYPE_ILL;
		return oplen;
	}

	r_strbuf_setf(&op->esil, "");
	handle_immediate_inst(&ctx, insn, mb_op);

	if (ctx.immfound_addr == ctx.op->addr - 4) ctx.immfound = true;
	else ctx.immfound = false;

	switch (mb_op->instr_type) {
	case arithmetic_inst:
		analyse_arithmetic_inst(&ctx, insn, mb_op);
		break;
	case logical_inst:
		analyse_logical_inst(&ctx, insn, mb_op);
		break;
	case mult_inst:
		analyse_mult_inst(&ctx, insn, mb_op);
		break;
	case div_inst:
		analyse_div_inst(&ctx, insn, mb_op);
		break;
	case branch_inst:
		analyse_branch_inst(&ctx, insn, mb_op);
		break;
	case return_inst:
		analyse_return_inst(&ctx, insn, mb_op);
		break;
	case special_inst:
		analyse_special_inst(&ctx, insn, mb_op);
		break;
	case memory_load_inst:
		analyse_memory_load_inst(&ctx, insn, mb_op);
		break;
	case memory_store_inst:
		analyse_memory_store_inst(&ctx, insn, mb_op);
		break;
	case barrel_shift_inst:
		analyse_barrel_shift_inst(&ctx, insn, mb_op);
		break;
	case anyware_inst:
		analyse_anyware_inst(&ctx, insn, mb_op);
		break;
	default:
		break;
	}

	return oplen;
}

static int archinfo(RAnal *anal, int q) {
	return 4;
}

static int microblaze_set_reg_profile(RAnal* anal) {
	const char *p =
		"=PC	pc\n"
		"=SP    r1\n"
		"=A1    r5\n"
		"=A2    r6\n"
		"=A3    r7\n"
		"=A4    r8\n"
		"=A5    r9\n"
		"=A6    r10\n"
		"=R0    r3\n"
		"=R1    r4\n"
		"gpr	r0	.32	0	0\n"
		"gpr	r1	.32	4	0\n"
		"gpr	r2	.32	8	0\n"
		"gpr	r3	.32	12	0\n"
		"gpr	r4	.32	16	0\n"
		"gpr	r5	.32	20	0\n"
		"gpr	r6	.32	24	0\n"
		"gpr	r7	.32	28	0\n"
		"gpr	r8	.32	32	0\n"
		"gpr	r9	.32	36	0\n"
		"gpr	r10	.32	40	0\n"
		"gpr	r11	.32	44	0\n"
		"gpr	r12	.32	48	0\n"
		"gpr	r13	.32	52	0\n"
		"gpr	r14	.32	56	0\n"
		"gpr	r15	.32	60	0\n"
		"gpr	r16	.32	64	0\n"
		"gpr	r17	.32	68	0\n"
		"gpr	r18	.32	72	0\n"
		"gpr	r19	.32	76	0\n"
		"gpr	r20	.32	80	0\n"
		"gpr	r21	.32	84	0\n"
		"gpr	r22	.32	88	0\n"
		"gpr	r23	.32	92	0\n"
		"gpr	r24	.32	96	0\n"
		"gpr	r25	.32	100	0\n"
		"gpr	r26	.32	104	0\n"
		"gpr	r27	.32	108	0\n"
		"gpr	r28	.32	112	0\n"
		"gpr	r29	.32	116	0\n"
		"gpr	r30	.32	120	0\n"
		"gpr	r31	.32	124	0\n"
		"gpr    pc      .32     128     0\n"
		"gpr    msr    .32     132     0\n" /* machine status register */
		"gpr    ear    .32     136     0\n" /* exception address register */
		"gpr    esr    .32     140     0\n" /* exception status register */
		"gpr    btr    .32     144     0\n" /* branch target register */
		"gpr    fsr    .32     148     0\n" /* floating point status register */
		"gpr    slr    .32     152     0\n" /* stack low register */
		"gpr    shr    .32     156     0\n";/* stack high register */

	return r_reg_set_profile_string(anal->reg, p);
}

struct r_anal_plugin_t r_anal_plugin_microblaze_gnu = {
	.name = "microblaze.gnu",
	.desc = "MICROBLAZE code analysis plugin",
	.license = "LGPL3",
	.arch = "microblaze",
	.bits = 32,
	.esil = true,
	.archinfo = archinfo,
	.op = &microblaze_op,
	.set_reg_profile = microblaze_set_reg_profile,
};

#ifndef CORELIB
struct r_lib_struct_t radare_plugin = {
        .type = R_LIB_TYPE_ANAL,
        .data = &r_anal_plugin_microblaze_gnu,
        .version = R2_VERSION
};
#endif
