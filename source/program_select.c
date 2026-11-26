#include "program_select.h"

#include <stdio.h>
#include <string.h>

bool is_switch2_program_id(uint64_t id) {
    return (id & 0xffff000000000000ULL) == 0x0400000000000000ULL;
}

int preferred_nup_program_slot(const GameProgramList *list,
                               uint64_t application_id) {
    if (!list || list->count <= 0) return -1;
    if (list->count == 1) return 0;

    /* 1. Prefer a native Switch 2 program (0x0400... runtime ID), matching
       the metadata platform when available or picking the first non-zero
       Switch 2 program index. */
    int switch2_slot = -1;
    for (int i = 0; i < list->count; ++i) {
        if (!is_switch2_program_id(list->runtime_ids[i])) continue;
        if (list->platform != PROGRAM_PLATFORM_UNKNOWN &&
            list->indices[i] == list->platform)
            return i;
        if (switch2_slot < 0 ||
            (list->indices[switch2_slot] == 0 && list->indices[i] > 0))
            switch2_slot = i;
    }
    if (switch2_slot >= 0) return switch2_slot;

    /* 2. When NCM reported a platform value, match its id_offset (e.g. 1 for
       Switch 2 Editions, 0 for Switch 1 multi-program collections). */
    if (list->platform != PROGRAM_PLATFORM_UNKNOWN) {
        for (int i = 0; i < list->count; ++i) {
            if (list->indices[i] == list->platform) return i;
        }
    }

    /* 3. If platform is unavailable, prefer a non-zero program whose runtime
       ID differs from the base application ID. */
    for (int i = 0; i < list->count; ++i) {
        if (list->indices[i] > 0 && list->runtime_ids[i] != 0 &&
            list->runtime_ids[i] != application_id)
            return i;
    }

    /* 4. Fallback to the first program (typically program 0). */
    return 0;
}

int resolve_nup_program_slot(const GameProgramList *list,
                             uint64_t application_id,
                             uint8_t preferred_program_index) {
    if (!list || list->count <= 0) return -1;
    if (preferred_program_index != PROGRAM_INDEX_AUTO) {
        for (int i = 0; i < list->count; ++i) {
            if (list->indices[i] == preferred_program_index) return i;
        }
    }
    return preferred_nup_program_slot(list, application_id);
}

void format_nup_program_label(const GameProgramList *list, int slot,
                              char *out, size_t out_size) {
    if (!out || !out_size) return;
    if (!list || slot < 0 || slot >= list->count) {
        snprintf(out, out_size, "Program 0");
        return;
    }
    const uint8_t index = list->indices[slot];
    const uint64_t runtime_id = list->runtime_ids[slot];
    const bool is_s2 = is_switch2_program_id(runtime_id) ||
                       (runtime_id == 0 &&
                        list->platform != PROGRAM_PLATFORM_UNKNOWN &&
                        list->platform > 0 && index == list->platform);
    if (is_s2) {
        snprintf(out, out_size, "Program %u (Switch 2)", index);
    } else if (index == 0 ||
               (runtime_id & 0xffff000000000000ULL) == 0x0100000000000000ULL) {
        snprintf(out, out_size, "Program %u (Switch 1)", index);
    } else {
        snprintf(out, out_size, "Program %u", index);
    }
}

size_t plan_program_inputs(ProgramPlanInput *out, size_t capacity,
                           uint32_t source_id, uint64_t default_title_id,
                           uint32_t version, uint8_t base_kind,
                           uint8_t base_layout, uint8_t extra_layout,
                           bool nup_format, const GameProgramList *programs,
                           uint8_t selected_program_index,
                           uint8_t *chosen_program_out) {
    if (chosen_program_out) *chosen_program_out = PROGRAM_INDEX_AUTO;
    if (!out || !capacity) return 0;

    if (!programs || programs->count <= 1) {
        out[0] = (ProgramPlanInput){
            .title_id = default_title_id,
            .version = version,
            .source_id = source_id,
            .kind = base_kind,
            .layout = base_layout,
            .program_index = PROGRAM_INDEX_AUTO,
        };
        return 1;
    }

    if (nup_format) {
        /* NUP SPEC §3.1.2 allows one base package per container. Pack the
           chosen program (Switch 2 native program1 by default) with its own
           runtime program ID (e.g. 0400... for Switch 2). */
        int slot = resolve_nup_program_slot(programs, default_title_id,
                                            selected_program_index);
        if (slot < 0) slot = 0;
        const uint8_t chosen = programs->indices[slot];
        const uint64_t runtime_id = programs->runtime_ids[slot];
        out[0] = (ProgramPlanInput){
            .title_id = runtime_id ? runtime_id : default_title_id,
            .version = version,
            .source_id = source_id,
            .kind = base_kind,
            .layout = base_layout,
            .program_index = chosen,
        };
        if (chosen_program_out) *chosen_program_out = chosen;
        return 1;
    }

    /* Extracted and TAR outputs preserve every program: program 0 in
       base_layout (game/ or base/), every other program in extra_layout
       (game/program<N>/ or base/program<N>/). */
    size_t written = 0;
    for (int i = 0; i < programs->count && written < capacity; ++i) {
        const uint8_t index = programs->indices[i];
        out[written++] = (ProgramPlanInput){
            .title_id = default_title_id,
            .version = version,
            .source_id = source_id,
            .kind = base_kind,
            .layout = index ? extra_layout : base_layout,
            .program_index = index,
        };
    }
    return written;
}
