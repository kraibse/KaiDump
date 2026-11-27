#ifndef KAIDUMP_PROGRAM_SELECT_H
#define KAIDUMP_PROGRAM_SELECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MAX_GAME_PROGRAMS 8
#define PROGRAM_INDEX_AUTO 0xff
#define PROGRAM_PLATFORM_UNKNOWN 0xff

typedef struct {
    uint8_t indices[MAX_GAME_PROGRAMS];
    uint64_t runtime_ids[MAX_GAME_PROGRAMS];
    int count;
    uint8_t platform;
} GameProgramList;

typedef struct {
    uint64_t title_id;
    uint32_t version;
    uint32_t source_id;
    uint8_t kind;
    uint8_t layout;
    uint8_t program_index;
} ProgramPlanInput;

bool is_switch2_program_id(uint64_t id);

/* Returns the slot [0..list->count-1] of the preferred program to pack in a
   single-base NUP (Switch 2 native program when present, otherwise platform
   match or program 0), or -1 if the list is empty. */
int preferred_nup_program_slot(const GameProgramList *list,
                               uint64_t application_id);

/* Resolves preferred_program_index against list, falling back to
   preferred_nup_program_slot() when unset or no longer present. */
int resolve_nup_program_slot(const GameProgramList *list,
                             uint64_t application_id,
                             uint8_t preferred_program_index);

/* Formats a human-readable label for the Output screen Program row, e.g.
   "Program 1 (Switch 2)" or "Program 0 (Switch 1)". */
void format_nup_program_label(const GameProgramList *list, int slot,
                              char *out, size_t out_size);

/* Appends the package input(s) for a game or base choice into out[0..capacity-1].
   In NUP mode with multiple programs, emits one package (kind=base_kind) for
   the chosen program using its runtime program ID (0400... for Switch 2).
   In TAR/Extracted mode with multiple programs, emits one entry per program
   (program 0 with base_layout, program N>0 with extra_layout).
   Returns the number of inputs appended. */
size_t plan_program_inputs(ProgramPlanInput *out, size_t capacity,
                           uint32_t source_id, uint64_t default_title_id,
                           uint32_t version, uint8_t base_kind,
                           uint8_t base_layout, uint8_t extra_layout,
                           bool nup_format, const GameProgramList *programs,
                           uint8_t selected_program_index,
                           uint8_t *chosen_program_out);

#endif
