/*
 * unwind.h: Stack Unwinding Interface
 *
 * Authors:
 *   Zoltan Varga (vargaz@gmail.com)
 *
 * (C) 2007 Novell, Inc.
 */

#ifndef __MONO_UNWIND_H__
#define __MONO_UNWIND_H__

#include "mini.h"

/*
 * This is a platform-independent interface for unwinding through stack frames 
 * based on the Dwarf unwinding interface.
 * See http://dwarfstd.org/Dwarf3.pdf, section "Call Frame Information".
 * Currently, this is only used for emitting unwind info in AOT files.
 */

/* CFA = Canonical Frame Address */

/* Unwind ops */

/* The low 6 bits contain additional information */
#define DW_CFA_advance_loc        0x40
#define DW_CFA_offset             0x80
#define DW_CFA_restore            0xc0

#define DW_CFA_nop              0x00
#define DW_CFA_set_loc          0x01
#define DW_CFA_advance_loc1     0x02
#define DW_CFA_advance_loc2     0x03
#define DW_CFA_advance_loc4     0x04
#define DW_CFA_offset_extended  0x05
#define DW_CFA_restore_extended 0x06
#define DW_CFA_undefined        0x07
#define DW_CFA_same_value       0x08
#define DW_CFA_register         0x09
#define DW_CFA_remember_state   0x0a
#define DW_CFA_restore_state    0x0b
#define DW_CFA_def_cfa          0x0c
#define DW_CFA_def_cfa_register 0x0d
#define DW_CFA_def_cfa_offset   0x0e
#define DW_CFA_def_cfa_expression 0x0f
#define DW_CFA_expression       0x10
#define DW_CFA_offset_extended_sf 0x11
#define DW_CFA_def_cfa_sf       0x12
#define DW_CFA_def_cfa_offset_sf 0x13
#define DW_CFA_val_offset        0x14
#define DW_CFA_val_offset_sf     0x15
#define DW_CFA_val_expression    0x16
#define DW_CFA_lo_user           0x1c
#define DW_CFA_hi_user           0x3f

/* Represents one unwind instruction */
typedef struct {
	guint8 op; /* One of DW_CFA_... */
	guint16 reg; /* register number in the hardware encoding */
	gint32 val; /* arbitrary value */
	guint32 when; /* The offset _after_ the cpu instruction this unwind op belongs to */
} MonoUnwindOp;

/* 
 * Macros for emitting MonoUnwindOp structures.
 * These should be called _after_ emitting the cpu instruction the unwind op
 * belongs to.
 */

/* Set cfa to reg+offset */
#define mono_emit_unwind_op_def_cfa(cfg,ip,reg,offset) mono_emit_unwind_op (cfg, (ip) - (cfg)->native_code, DW_CFA_def_cfa, (reg), (offset))
/* Set cfa to reg+existing offset */
#define mono_emit_unwind_op_def_cfa_reg(cfg,ip,reg) mono_emit_unwind_op (cfg, (ip) - (cfg)->native_code, DW_CFA_def_cfa_register, (reg), (0))
/* Set cfa to existing reg+offset */
#define mono_emit_unwind_op_def_cfa_offset(cfg,ip,offset) mono_emit_unwind_op (cfg, (ip) - (cfg)->native_code, DW_CFA_def_cfa_offset, (0), (offset))
/* Reg is the same as it was on enter to the function */
#define mono_emit_unwind_op_same_value(cfg,ip,reg) mono_emit_unwind_op (cfg, (ip) - (cfg)->native_code, DW_CFA_same_value, (reg), 0)
/* Reg is saved at cfa+offset */
#define mono_emit_unwind_op_offset(cfg,ip,reg,offset) mono_emit_unwind_op (cfg, (ip) - (cfg)->native_code, DW_CFA_offset, (reg), (offset))

/* Similar macros usable when a cfg is not available, like for trampolines */
#define mono_add_unwind_op_def_cfa(op_list,code,buf,reg,offset) do { (op_list) = g_slist_append ((op_list), mono_create_unwind_op ((code) - (buf), DW_CFA_def_cfa, (reg), (offset))); } while (0)
#define mono_add_unwind_op_def_cfa_reg(op_list,code,buf,reg) do { (op_list) = g_slist_append ((op_list), mono_create_unwind_op ((code) - (buf), DW_CFA_def_cfa_register, (reg), (0))); } while (0)
#define mono_add_unwind_op_def_cfa_offset(op_list,code,buf,offset) do { (op_list) = g_slist_append ((op_list), mono_create_unwind_op ((code) - (buf), DW_CFA_def_cfa_offset, 0, (offset))); } while (0)
#define mono_add_unwind_op_same_value(op_list,code,buf,reg) do { (op_list) = g_slist_append ((op_list), mono_create_unwind_op ((code) - (buf), DW_CFA_same_value, (reg), 0)); } while (0)
#define mono_add_unwind_op_offset(op_list,code,buf,reg,offset) do { (op_list) = g_slist_append ((op_list), mono_create_unwind_op ((code) - (buf), DW_CFA_offset, (reg), (offset))); } while (0)

int
mono_hw_reg_to_dwarf_reg (int reg) MONO_INTERNAL;

int
mono_unwind_get_dwarf_data_align (void) MONO_INTERNAL;

int
mono_unwind_get_dwarf_pc_reg (void) MONO_INTERNAL;

guint8*
mono_unwind_ops_encode (GSList *unwind_ops, guint32 *out_len) MONO_INTERNAL;

void
mono_unwind_frame (guint8 *unwind_info, guint32 unwind_info_len, 
				   guint8 *start_ip, guint8 *end_ip, guint8 *ip, mgreg_t *regs, 
				   int nregs, guint8 **out_cfa) MONO_INTERNAL;

void mono_unwind_init (void) MONO_INTERNAL;

void mono_unwind_cleanup (void) MONO_INTERNAL;

guint32 mono_cache_unwind_info (guint8 *unwind_info, guint32 unwind_info_len) MONO_INTERNAL;

guint8* mono_get_cached_unwind_info (guint32 index, guint32 *unwind_info_len) MONO_INTERNAL;

guint8* mono_unwind_get_ops_from_fde (guint8 *fde, guint32 *out_len, guint32 *code_len) MONO_INTERNAL;

#endif
