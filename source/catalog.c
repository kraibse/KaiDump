#include "app_internal.h"

/* Title discovery and presentation metadata. */

void copy_display_text(char *out, size_t out_size, const char *input) {
    size_t at = 0;
    if (!out_size) return;
    while (*input && at + 1 < out_size) {
        u32 codepoint = 0;
        ssize_t consumed = decode_utf8(&codepoint, (const u8 *)input);
        if (consumed <= 0) {
            codepoint = '?';
            consumed = 1;
        }
        if (codepoint < 0x20 || codepoint == 0x7f) codepoint = ' ';
        if ((codepoint >= 0x2010 && codepoint <= 0x2015) ||
            codepoint == 0x2212) codepoint = '-';
        else if (codepoint == 0x2018 || codepoint == 0x2019)
            codepoint = '\'';
        else if (codepoint == 0x201c || codepoint == 0x201d)
            codepoint = '"';
        if (codepoint == 0x2026 && at + 3 < out_size) {
            out[at++] = '.';
            out[at++] = '.';
            out[at++] = '.';
        } else if (codepoint <= 0x7f) {
            out[at++] = (char)codepoint;
        } else {
            if (at + (size_t)consumed >= out_size) break;
            memcpy(out + at, input, (size_t)consumed);
            at += (size_t)consumed;
        }
        input += consumed;
    }
    while (at && out[at - 1] == ' ') --at;
    out[at] = 0;
}

void build_output_name(const Application *app, const char *extension,
                              char *output, size_t output_size) {
    const char *input = app->title[0] ? app->title : "Title unavailable";
    char safe_title[221];
    size_t at = 0;
    while (*input && at + 1 < sizeof(safe_title)) {
        u32 codepoint = 0;
        ssize_t consumed = decode_utf8(&codepoint, (const u8 *)input);
        if (consumed <= 0) {
            codepoint = '_';
            consumed = 1;
        }
        if (codepoint <= 0x7f) {
            char value = (char)codepoint;
            if (codepoint < 0x20 || codepoint == 0x7f ||
                strchr("<>:\"/\\|?*", value))
                value = '_';
            safe_title[at++] = value;
        } else {
            if (at + (size_t)consumed >= sizeof(safe_title)) break;
            memcpy(safe_title + at, input, (size_t)consumed);
            at += (size_t)consumed;
        }
        input += consumed;
    }
    while (at && (safe_title[at - 1] == ' ' || safe_title[at - 1] == '.'))
        --at;
    safe_title[at] = 0;
    if (!safe_title[0]) snprintf(safe_title, sizeof(safe_title), "Title");
    snprintf(output, output_size, "%s [%016llX]%s", safe_title,
             (unsigned long long)app->record.application_id,
             extension ? extension : "");
}

static Result load_control_from_readonly_ns(Application *app) {
    Service ns = {0};
    Service reader = {0};
    Result result = smGetService(&ns, "ns:ro");
    if (R_FAILED(result)) result = smGetService(&ns, "ns:am2");
    if (R_FAILED(result)) {
        log_result("control-readonly-service", result);
        return result;
    }
    result = serviceDispatch(&ns, 7989,
        .out_num_objects = 1,
        .out_objects = &reader,
    );
    serviceClose(&ns);
    if (R_FAILED(result)) {
        log_result("control-readonly-open", result);
        return result;
    }

    const size_t buffer_size = 0x24000;
    void *buffer = calloc(1, buffer_size);
    if (!buffer) {
        serviceClose(&reader);
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }
    const struct {
        u8 source;
        u8 padding[7];
        u64 application_id;
    } input = {
        NsApplicationControlSource_Storage,
        {0},
        app->record.application_id,
    };
    result = serviceDispatchIn(&reader, 0, input,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { buffer, buffer_size } },
    );
    log_line("control-readonly title=%016llX result=0x%08x",
             (unsigned long long)app->record.application_id, result);
    if (R_SUCCEEDED(result)) title_from_nacp(app, (NacpStruct *)buffer);
    free(buffer);
    serviceClose(&reader);
    return R_SUCCEEDED(result) && app->title[0]
        ? 0 : (R_FAILED(result) ? result
                               : MAKERESULT(Module_Libnx, LibnxError_NotFound));
}

static void load_metadata(Application *app) {
    if (app->metadata_loaded || !g.ns_active) return;

    app->metadata_loaded = true;
    u64 actual_size = 0;
    memset(&g.control_data, 0, sizeof(g.control_data));
    app->metadata_result = nsGetApplicationControlData(
        NsApplicationControlSource_Storage,
        app->record.application_id,
        &g.control_data,
        sizeof(g.control_data),
        &actual_size);
    if (R_FAILED(app->metadata_result) && hosversionAtLeast(19, 0, 0)) {
        u32 unknown = 0;
        memset(&g.control_data, 0, sizeof(g.control_data));
        app->metadata_result = nsGetApplicationControlData2(
            NsApplicationControlSource_Storage,
            app->record.application_id,
            &g.control_data,
            sizeof(g.control_data),
            0,
            1,
            &actual_size,
            &unknown);
        log_line("control2 title=%016llX acd=1 result=0x%08x size=%llu unknown=%u",
                 (unsigned long long)app->record.application_id,
                 app->metadata_result, (unsigned long long)actual_size, unknown);
    }
    log_line("control title=%016llX result=0x%08x size=%llu format=%u",
             (unsigned long long)app->record.application_id,
             app->metadata_result, (unsigned long long)actual_size,
             R_SUCCEEDED(app->metadata_result) ? g.control_data.nacp.titles_data_format : 0);
    if (R_FAILED(app->metadata_result) || actual_size < sizeof(NacpStruct)) {
        Result readonly_result = load_control_from_readonly_ns(app);
        if (R_SUCCEEDED(readonly_result)) {
            log_line("control-readonly-name title=%016llX name=%s",
                     (unsigned long long)app->record.application_id, app->title);
            return;
        }
        Result content_result = load_control_from_content(app);
        log_line("control-content title=%016llX result=0x%08x name=%s",
                 (unsigned long long)app->record.application_id,
                 content_result, app->title[0] ? app->title : "(unavailable)");
        return;
    }

    NacpLanguageEntry *language = NULL;
    Result language_result = nsGetApplicationDesiredLanguage(
        &g.control_data.nacp, &language);
    if (R_FAILED(language_result)) {
        language_result = nacpGetLanguageEntry(&g.control_data.nacp, &language);
    }
    if (!app->title[0] && R_SUCCEEDED(language_result) && language) {
        copy_display_text(app->title, sizeof(app->title), language->name);
    }
    copy_display_text(app->version, sizeof(app->version),
                      g.control_data.nacp.display_version);
    log_line("control-language title=%016llX result=0x%08x name=%s",
             (unsigned long long)app->record.application_id, language_result,
             app->title[0] ? app->title : "(unavailable)");
}

static void load_title_list(void) {
    g.title_list_result = MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer);
    if (g.count <= 0 || hosversionBefore(20, 0, 0)) return;

    const size_t entry_bytes = (size_t)g.count * sizeof(NacpLanguageEntry);
    size_t buffer_size = entry_bytes + (size_t)g.count * sizeof(u64) +
                         sizeof(NsApplicationControlData);
    buffer_size = (buffer_size + 0xfff) & ~(size_t)0xfff;
    void *buffer = aligned_alloc(0x1000, buffer_size);
    u64 *ids = malloc((size_t)g.count * sizeof(*ids));
    if (!buffer || !ids) {
        g.title_list_result = MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
        free(buffer);
        free(ids);
        log_result("title-list allocation", g.title_list_result);
        return;
    }
    memset(buffer, 0, buffer_size);
    for (s32 i = 0; i < g.count; ++i) ids[i] = g.applications[i].record.application_id;

    AsyncValue value = {0};
    g.title_list_result = nsListApplicationTitle2(
        &value, NsApplicationControlSource_Storage, ids, g.count, buffer, buffer_size);
    s32 offset = -1;
    u64 value_size = 0;
    if (R_SUCCEEDED(g.title_list_result))
        g.title_list_result = asyncValueWait(&value, UINT64_MAX);
    if (R_SUCCEEDED(g.title_list_result))
        g.title_list_result = asyncValueGetSize(&value, &value_size);
    if (R_SUCCEEDED(g.title_list_result) && value_size < entry_bytes)
        g.title_list_result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    if (R_SUCCEEDED(g.title_list_result))
        g.title_list_result = asyncValueGet(&value, &offset, sizeof(offset));

    if (R_SUCCEEDED(g.title_list_result) && offset >= 0 &&
        (size_t)offset <= buffer_size && entry_bytes <= buffer_size - (size_t)offset) {
        NacpLanguageEntry *entries = (NacpLanguageEntry *)((u8 *)buffer + offset);
        s32 named = 0;
        for (s32 i = 0; i < g.count; ++i)
            copy_display_text(g.applications[i].title,
                              sizeof(g.applications[i].title), entries[i].name);
        for (s32 i = 0; i < g.count; ++i)
            if (g.applications[i].title[0]) ++named;
        log_line("title-list supplied names=%d/%d", named, g.count);
    } else if (R_SUCCEEDED(g.title_list_result)) {
        g.title_list_result = MAKERESULT(Module_Libnx, LibnxError_BadInput);
    }
    log_line("title-list result=0x%08x count=%d buffer=%zu value=%llu offset=%d",
             g.title_list_result, g.count, buffer_size,
             (unsigned long long)value_size, offset);
    if (serviceIsActive(&value.s)) asyncValueClose(&value);
    free(ids);
    free(buffer);
}

static int compare_applications(const void *left, const void *right) {
    const Application *a = left;
    const Application *b = right;
    if (!a->title[0] || !b->title[0]) {
        if (a->title[0] != b->title[0]) return a->title[0] ? -1 : 1;
    } else {
        const unsigned char *a_text = (const unsigned char *)a->title;
        const unsigned char *b_text = (const unsigned char *)b->title;
        while (*a_text && *b_text) {
            unsigned char a_value = *a_text;
            unsigned char b_value = *b_text;
            if (a_value < 0x80) a_value = (unsigned char)tolower(a_value);
            if (b_value < 0x80) b_value = (unsigned char)tolower(b_value);
            if (a_value != b_value) return a_value < b_value ? -1 : 1;
            ++a_text;
            ++b_text;
        }
        if (*a_text != *b_text) return *a_text ? 1 : -1;
        int exact = strcmp(a->title, b->title);
        if (exact) return exact;
    }
    if (a->record.application_id == b->record.application_id) return 0;
    return a->record.application_id < b->record.application_id ? -1 : 1;
}

static void sort_applications(void) {
    if (g.count > 1)
        qsort(g.applications, (size_t)g.count,
              sizeof(g.applications[0]), compare_applications);
    log_line("application-sort count=%d", g.count);
}

void load_visible_metadata(void) {
    if (g.count <= 0) return;
    s32 first = (g.selected / ROWS_PER_PAGE) * ROWS_PER_PAGE;
    s32 last = first + ROWS_PER_PAGE;
    if (last > g.count) last = g.count;
    for (s32 i = first; i < last; ++i) load_metadata(&g.applications[i]);
}

static Result discover_accessible_applications(void) {
    Result result = ncmInitialize();
    log_result("inventory-ncm-initialize", result);
    if (R_FAILED(result)) return result;

    const u8 storages[] = {
        NcmStorageId_BuiltInUser,
        NcmStorageId_SdCard,
        NcmStorageId_GameCard,
    };
    bool opened_any = false;
    Result last = MAKERESULT(Module_Libnx, LibnxError_NotFound);
    for (size_t storage_index = 0;
         storage_index < sizeof(storages); ++storage_index) {
        NcmContentMetaDatabase database = {0};
        last = ncmOpenContentMetaDatabase(&database, storages[storage_index]);
        log_line("inventory-open storage=%u result=0x%08x",
                 storages[storage_index], last);
        if (R_FAILED(last)) continue;
        opened_any = true;

        NcmContentMetaKey keys[MAX_APPLICATIONS] = {0};
        s32 total = 0;
        s32 written = 0;
        last = ncmContentMetaDatabaseList(
            &database, &total, &written, keys, MAX_APPLICATIONS,
            NcmContentMetaType_Application, 0, 0, UINT64_MAX,
            NcmContentInstallType_Full);
        log_line("inventory-list storage=%u result=0x%08x total=%d written=%d",
                 storages[storage_index], last, total, written);
        if (R_SUCCEEDED(last)) {
            if (written > MAX_APPLICATIONS) written = MAX_APPLICATIONS;
            for (s32 i = 0; i < written && g.count < MAX_APPLICATIONS; ++i) {
                bool duplicate = false;
                for (s32 existing = 0; existing < g.count; ++existing) {
                    if (g.applications[existing].record.application_id == keys[i].id) {
                        if (storages[storage_index] == NcmStorageId_GameCard) {
                            g.applications[existing].base_storage =
                                NcmStorageId_GameCard;
                            g.applications[existing].base_version =
                                keys[i].version;
                        }
                        duplicate = true;
                        break;
                    }
                }
                if (duplicate) continue;
                Application *app = &g.applications[g.count++];
                app->record.application_id = keys[i].id;
                app->base_storage = storages[storage_index];
                app->base_version = keys[i].version;
                log_line("inventory-add title=%016llX storage=%u version=%u",
                         (unsigned long long)keys[i].id,
                         storages[storage_index], keys[i].version);
            }
        }
        ncmContentMetaDatabaseClose(&database);
    }
    ncmExit();
    if (g.count > 0) return 0;
    return opened_any ? last : MAKERESULT(Module_Libnx, LibnxError_NotFound);
}

void refresh_applications(void) {
    close_ns();
    uiClearThumbnails();
    memset(g.applications, 0, sizeof(g.applications));
    memset(g.grid_icon_loaded, 0, sizeof(g.grid_icon_loaded));
    g.grid_icon_first = -1;
    g.count = 0;
    g.selected = 0;
    g.meta_count = 0;
    g.list_result = 0;
    g.screen = SCREEN_TITLES;

    g.init_result = nsInitialize();
    log_result("nsInitialize", g.init_result);
    if (R_FAILED(g.init_result)) return;
    g.ns_active = true;

    g.list_result = discover_accessible_applications();
    if (R_FAILED(g.list_result)) {
        g.count = 0;
        return;
    }
    log_line("accessible-application-list result=0x%08x count=%d",
             g.list_result, g.count);
    load_title_list();
    for (s32 i = 0; i < g.count; ++i) {
        if (!g.applications[i].title[0])
            load_metadata(&g.applications[i]);
    }
    sort_applications();
    load_visible_metadata();
}

const char *storage_name(u8 id) {
    switch (id) {
        case NcmStorageId_GameCard: return "Game card";
        case NcmStorageId_BuiltInSystem: return "System";
        case NcmStorageId_BuiltInUser: return "Internal";
        case NcmStorageId_SdCard: return "SD card";
        default: return "Unknown";
    }
}

static void add_content_choice(ContentKind kind, s32 meta_index,
                               s32 update_index, bool selected) {
    if (g.choice_count >= MAX_META_STATUS) return;
    g.choices[g.choice_count++] = (ContentChoice){
        .kind = kind,
        .meta_index = meta_index,
        .update_index = update_index,
        .selected = selected,
    };
}

static void build_content_choices(void) {
    g.choice_count = 0;
    /* The same entries the export routes use: installed, newest version.
       An update NS still lists without storage cannot be read, so it must
       not turn the game into "Base + installed update". */
    const NsApplicationContentMetaStatus *base_status =
        find_program_status(NcmContentMetaType_Application);
    const NsApplicationContentMetaStatus *update_status =
        find_program_status(NcmContentMetaType_Patch);
    s32 base = base_status ? (s32)(base_status - g.meta) : -1;
    s32 update = update_status ? (s32)(update_status - g.meta) : -1;
    for (s32 i = 0; i < g.meta_count && base < 0; ++i) {
        /* Keep a base NS lists without storage as before: the export
           routes still look it up through NCM. */
        if (g.meta[i].meta_type == NcmContentMetaType_Application) base = i;
    }
    for (s32 i = 0; i < g.meta_count; ++i) {
        if (g.meta[i].meta_type == NcmContentMetaType_Patch &&
            &g.meta[i] != update_status)
            log_line("content-update-ignored id=%016llX version=%u storage=%u",
                     (unsigned long long)g.meta[i].application_id,
                     g.meta[i].version, g.meta[i].storageID);
    }

    bool game_card = base >= 0 &&
                     g.applications[g.selected].base_storage ==
                         NcmStorageId_GameCard;
    if (game_card) {
        /* An update's RomFS is a delta on top of the base RomFS, so it can
           only be read merged with the base. Offer the merged game, plus the
           untouched game card as an option. A card can also ship with a
           revision at or above the installed update; the console ignores
           such an update, so only the card is offered then. */
        const bool update_newer = update >= 0 &&
            g.meta[update].version > g.meta[base].version;
        if (update >= 0 && !update_newer)
            log_line("content-update-superseded update=%u base=%u",
                     g.meta[update].version, g.meta[base].version);
        if (update_newer) add_content_choice(CONTENT_GAME, base, update, true);
        add_content_choice(CONTENT_BASE, base, -1, !update_newer);
    } else if (base >= 0) {
        add_content_choice(CONTENT_GAME, base, update, true);
    } else if (update >= 0) {
        add_content_choice(CONTENT_GAME, update, update, true);
    }

    for (s32 i = 0; i < g.meta_count; ++i) {
        const NsApplicationContentMetaStatus *status = &g.meta[i];
        if (status->meta_type == NcmContentMetaType_DataPatch) {
            /* Like a game update, a DLC update only holds changes on top of
               its DLC: it is merged into its DLC's row, never offered alone. */
            log_line("content-dlc-update id=%016llX version=%u storage=%u",
                     (unsigned long long)status->application_id,
                     status->version, status->storageID);
            continue;
        }
        if (status->meta_type != NcmContentMetaType_AddOnContent) continue;
        if (status->storageID == NcmStorageId_None) {
            /* Listed by NS but not installed, and not on the inserted card. */
            log_line("content-dlc-unavailable id=%016llX",
                     (unsigned long long)status->application_id);
            continue;
        }
        const NsApplicationContentMetaStatus *dlc_update =
            find_dlc_update(status);
        add_content_choice(CONTENT_DLC, i,
                           dlc_update ? (s32)(dlc_update - g.meta) : -1,
                           false);
    }
    log_line("content-choices count=%d base=%d update=%d game-card=%u",
             g.choice_count, base, update, game_card);
}

static void load_selected_icon(Application *app) {
    uiClearImage();
    g.selected_icon_loaded = false;
    if (!g.ns_active) return;

    if (g.control_icon_size) {
        g.selected_icon_loaded = uiSetJpegImage(
            g.control_icon, g.control_icon_size);
        log_line("details-icon title=%016llX source=content size=%zu loaded=%u",
                 (unsigned long long)app->record.application_id,
                 g.control_icon_size, g.selected_icon_loaded);
        if (g.selected_icon_loaded) return;
    }

    u64 actual_size = 0;
    Result result = nsGetApplicationControlData(
        NsApplicationControlSource_Storage, app->record.application_id,
        &g.control_data, sizeof(g.control_data), &actual_size);
    if (R_FAILED(result) && hosversionAtLeast(19, 0, 0)) {
        u32 unknown = 0;
        result = nsGetApplicationControlData2(
            NsApplicationControlSource_Storage, app->record.application_id,
            &g.control_data, sizeof(g.control_data), 0, 1,
            &actual_size, &unknown);
    }
    size_t icon_size = actual_size > sizeof(NacpStruct)
        ? (size_t)(actual_size - sizeof(NacpStruct)) : 0;
    if (icon_size > sizeof(g.control_data.icon))
        icon_size = sizeof(g.control_data.icon);
    bool loaded = R_SUCCEEDED(result) &&
                  uiSetJpegImage(g.control_data.icon, icon_size);
    g.selected_icon_loaded = loaded;
    log_line("details-icon title=%016llX source=ns result=0x%08x size=%zu loaded=%u",
             (unsigned long long)app->record.application_id,
             result, icon_size, loaded);
}

static bool load_grid_icon(Application *app, int slot) {
    u64 actual_size = 0;
    Result result = nsGetApplicationControlData(
        NsApplicationControlSource_Storage, app->record.application_id,
        &g.control_data, sizeof(g.control_data), &actual_size);
    if (R_FAILED(result) && hosversionAtLeast(19, 0, 0)) {
        u32 unknown = 0;
        result = nsGetApplicationControlData2(
            NsApplicationControlSource_Storage, app->record.application_id,
            &g.control_data, sizeof(g.control_data), 0, 1,
            &actual_size, &unknown);
    }
    size_t icon_size = actual_size > sizeof(NacpStruct)
        ? (size_t)(actual_size - sizeof(NacpStruct)) : 0;
    if (icon_size > sizeof(g.control_data.icon))
        icon_size = sizeof(g.control_data.icon);
    bool loaded = R_SUCCEEDED(result) && icon_size &&
                  uiSetJpegThumbnail(slot, g.control_data.icon, icon_size);
    const char *source = "ns";

    if (!loaded) {
        result = load_control_from_content(app);
        source = "content";
        loaded = R_SUCCEEDED(result) && g.control_icon_size &&
                 uiSetJpegThumbnail(slot, g.control_icon,
                                    g.control_icon_size);
    }
    log_line("grid-icon slot=%d title=%016llX source=%s result=0x%08x size=%zu loaded=%u",
             slot, (unsigned long long)app->record.application_id,
             source, result,
             source[0] == 'n' ? icon_size : g.control_icon_size, loaded);
    return loaded;
}

void load_grid_icons(void) {
    if (!g.title_grid || g.count <= 0) return;
    s32 first = (g.selected / ROWS_PER_PAGE) * ROWS_PER_PAGE;
    if (g.grid_icon_first == first) return;

    uiClearThumbnails();
    memset(g.grid_icon_loaded, 0, sizeof(g.grid_icon_loaded));
    g.grid_icon_first = first;
    s32 last = first + ROWS_PER_PAGE;
    if (last > g.count) last = g.count;
    for (s32 i = first; i < last; ++i) {
        int slot = i - first;
        g.grid_icon_loaded[slot] = load_grid_icon(&g.applications[i], slot);
    }
}

/* NS reports content on an inserted game card with no storage (base game,
   and the DLC that Switch 2 Edition cards carry). Look each one up in the
   card's own content meta database so it can be resolved and dumped. */
static void resolve_game_card_meta(void) {
    bool missing = false;
    for (s32 i = 0; i < g.meta_count; ++i)
        if (g.meta[i].storageID == NcmStorageId_None) missing = true;
    if (!missing) return;

    Result result = ncmInitialize();
    log_result("details-game-card-ncm-initialize", result);
    if (R_FAILED(result)) return;

    NcmContentMetaDatabase database = {0};
    result = ncmOpenContentMetaDatabase(&database, NcmStorageId_GameCard);
    log_line("details-game-card-open result=0x%08x", result);
    if (R_SUCCEEDED(result)) {
        for (s32 i = 0; i < g.meta_count; ++i) {
            NsApplicationContentMetaStatus *status = &g.meta[i];
            if (status->storageID != NcmStorageId_None) continue;
            NcmContentMetaKey key = {0};
            Result key_result = ncmContentMetaDatabaseGetLatestContentMetaKey(
                &database, &key, status->application_id);
            const bool match = R_SUCCEEDED(key_result) &&
                               key.type == status->meta_type;
            log_line("details-game-card-meta id=%016llX type=%u result=0x%08x found-type=%u ns-version=%u card-version=%u match=%u",
                     (unsigned long long)status->application_id,
                     status->meta_type, key_result,
                     R_SUCCEEDED(key_result) ? key.type : 0,
                     status->version,
                     R_SUCCEEDED(key_result) ? key.version : 0, match);
            if (!match) continue;
            status->storageID = NcmStorageId_GameCard;
            status->version = key.version;
        }
        ncmContentMetaDatabaseClose(&database);
    }
    ncmExit();
}

void load_details(void) {
    if (g.count <= 0) return;
    Application *app = &g.applications[g.selected];
    load_metadata(app);
    g.meta_count = 0;
    g.details_result = nsListApplicationContentMetaStatus(
        app->record.application_id, 0, g.meta, MAX_META_STATUS, &g.meta_count);
    if (R_FAILED(g.details_result)) g.meta_count = 0;
    if (g.meta_count > MAX_META_STATUS) g.meta_count = MAX_META_STATUS;
    for (s32 i = 0; i < g.meta_count; ++i) {
        if (g.meta[i].meta_type == NcmContentMetaType_Application &&
            g.meta[i].storageID == NcmStorageId_None && app->base_storage) {
            log_line("details-base-storage title=%016llX ns=%u inventory=%u ns-version=%u inventory-version=%u",
                     (unsigned long long)app->record.application_id,
                     g.meta[i].storageID, app->base_storage,
                     g.meta[i].version, app->base_version);
            g.meta[i].storageID = app->base_storage;
            /* NS does not know where this base lives, so its version may be
               stale too; NCM lookups need the version actually on storage. */
            g.meta[i].version = app->base_version;
        }
    }
    resolve_game_card_meta();
    Result current_control = load_control_from_content(app);
    log_line("details-current-control title=%016llX result=0x%08x version=%s icon=%zu",
             (unsigned long long)app->record.application_id,
             current_control, app->version[0] ? app->version : "(unavailable)",
             g.control_icon_size);
    g.content_cursor = 0;
    build_content_choices();
    load_selected_icon(app);
    g.screen = SCREEN_DETAILS;
    log_line("details title=%016llX result=0x%08x count=%d",
             (unsigned long long)app->record.application_id,
             g.details_result, g.meta_count);
}

/* NUP application metadata (SPEC §5; type numbers in nup.h) ---------------- */

/* NACP language index -> BCP-47 tag (SPEC Appendix B). */
static const char *const g_nacp_language_tags[16] = {
    "en-us", "en-gb", "ja", "fr", "de", "es-419", "es", "it",
    "nl", "fr-ca", "pt", "ru", "ko", "zh-hant", "zh-hans", "pt-br",
};

typedef struct {
    uint8_t *data;
    size_t size;
    size_t capacity;
    bool failed;
} MetaBuffer;

static void meta_append(MetaBuffer *buffer, const void *data, size_t size) {
    if (buffer->failed || !size) return;
    if (buffer->size + size > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity : 4096;
        while (capacity < buffer->size + size) capacity *= 2;
        uint8_t *grown = realloc(buffer->data, capacity);
        if (!grown) {
            buffer->failed = true;
            return;
        }
        buffer->data = grown;
        buffer->capacity = capacity;
    }
    memcpy(buffer->data + buffer->size, data, size);
    buffer->size += size;
}

static void meta_tlv(MetaBuffer *buffer, uint16_t type, const void *data,
                     uint32_t size) {
    const uint8_t header[8] = {
        (uint8_t)type, (uint8_t)(type >> 8), 0, 0,
        (uint8_t)size, (uint8_t)(size >> 8), (uint8_t)(size >> 16),
        (uint8_t)(size >> 24),
    };
    meta_append(buffer, header, sizeof(header));
    meta_append(buffer, data, size);
}

bool build_nup_app_meta(bool include_version, uint8_t **out_meta,
                        size_t *out_size) {
    *out_meta = NULL;
    *out_size = 0;
    if (g.count <= 0) return false;
    Application *app = &g.applications[g.selected];

    /* The content route works for Switch 2 titles; NS for older ones. */
    const uint8_t *icon = NULL;
    size_t icon_size = 0;
    bool nacp_valid = false;
    g_capture_control_icons = true;
    Result result = load_control_from_content(app);
    g_capture_control_icons = false;
    if (R_FAILED(result)) free_control_icons();
    if (R_SUCCEEDED(result)) {
        nacp_valid = true;
        if (g.control_icon && g.control_icon_size) {
            icon = g.control_icon;
            icon_size = g.control_icon_size;
        }
        /* The default icon (TLV 0x0003) is the American English one. */
        for (int i = 0; i < g_control_icon_count; ++i) {
            if (strcmp(g_control_icons[i].language, "AmericanEnglish"))
                continue;
            icon = g_control_icons[i].data;
            icon_size = g_control_icons[i].size;
        }
    }
    if (!icon) {
        u64 actual = 0;
        Result ns_result = nsGetApplicationControlData(
            NsApplicationControlSource_Storage, app->record.application_id,
            &g.control_data, sizeof(g.control_data), &actual);
        if (R_SUCCEEDED(ns_result) && actual >= sizeof(g.control_data.nacp)) {
            nacp_valid = true;
            size_t size = (size_t)(actual - sizeof(g.control_data.nacp));
            if (size > sizeof(g.control_data.icon))
                size = sizeof(g.control_data.icon);
            if (size) {
                icon = g.control_data.icon;
                icon_size = size;
            }
        }
    }
    NacpStruct *nacp = &g.control_data.nacp;

    /* Per-language names: plain in older NACPs, zlib-compressed (32 entries)
       when titles_data_format is 1. */
    NacpLanguageEntry *entries = NULL;
    NacpLanguageEntry *inflated = NULL;
    if (nacp_valid && nacp->titles_data_format == 1) {
        const size_t output_size = 32 * sizeof(NacpLanguageEntry);
        inflated = calloc(1, output_size);
        if (inflated) {
            z_stream stream = {0};
            stream.next_in = nacp->lang_data.compressed_data.buffer;
            stream.avail_in = nacp->lang_data.compressed_data.buffer_size;
            stream.next_out = (Bytef *)inflated;
            stream.avail_out = output_size;
            int zresult = inflateInit2(&stream, -15);
            if (zresult == Z_OK) zresult = inflate(&stream, Z_FINISH);
            inflateEnd(&stream);
            if (zresult == Z_STREAM_END) entries = inflated;
        }
    } else if (nacp_valid) {
        entries = nacp->lang_data.lang;
    }

    const NacpLanguageEntry *primary = NULL;
    for (int i = 0; entries && i < 16 && !primary; ++i)
        if (entries[i].name[0]) primary = &entries[i];

    MetaBuffer buffer = {0};
    if (include_version && nacp_valid)
        meta_tlv(&buffer, NUP_TLV_NACP, nacp, sizeof(*nacp));
    if (icon_size) meta_tlv(&buffer, NUP_TLV_ICON, icon, (uint32_t)icon_size);
    size_t version_length = nacp_valid
        ? strnlen(nacp->display_version, sizeof(nacp->display_version)) : 0;
    if (include_version && version_length)
        meta_tlv(&buffer, NUP_TLV_DISPLAY_VERSION, nacp->display_version,
                 (uint32_t)version_length);
    if (primary) {
        meta_tlv(&buffer, NUP_TLV_TITLE_NAME, primary->name,
                 (uint32_t)strnlen(primary->name, sizeof(primary->name)));
    } else if (app->title[0]) {
        meta_tlv(&buffer, NUP_TLV_TITLE_NAME, app->title, (uint32_t)strlen(app->title));
    }
    if (primary && primary->author[0])
        meta_tlv(&buffer, NUP_TLV_PUBLISHER, primary->author,
                 (uint32_t)strnlen(primary->author, sizeof(primary->author)));

    /* Localized titles: lang_len, lang, data_len (u32 LE), data. */
    MetaBuffer localized = {0};
    int languages = 0;
    for (int i = 0; entries && i < 16; ++i) {
        size_t length = strnlen(entries[i].name, sizeof(entries[i].name));
        if (!length) continue;
        const char *tag = g_nacp_language_tags[i];
        const uint8_t tag_length = (uint8_t)strlen(tag);
        const uint8_t size[4] = {
            (uint8_t)length, (uint8_t)(length >> 8), (uint8_t)(length >> 16),
            (uint8_t)(length >> 24),
        };
        meta_append(&localized, &tag_length, 1);
        meta_append(&localized, tag, tag_length);
        meta_append(&localized, size, sizeof(size));
        meta_append(&localized, entries[i].name, length);
        ++languages;
    }
    if (languages && !localized.failed)
        meta_tlv(&buffer, NUP_TLV_LOCALIZED_TITLES, localized.data, (uint32_t)localized.size);
    free(localized.data);
    free(inflated);

    /* Localized icons: the same record layout, with the image bytes. */
    MetaBuffer localized_icons = {0};
    int icon_languages = 0;
    for (int i = 0; i < g_control_icon_count; ++i) {
        const ControlIcon *control_icon = &g_control_icons[i];
        const char *tag = nupLanguageTag(control_icon->language);
        if (!tag) {
            log_line("nup-meta-icon-skip language=%s reason=unknown-language",
                     control_icon->language);
            continue;
        }
        const uint8_t tag_length = (uint8_t)strlen(tag);
        const uint32_t length = (uint32_t)control_icon->size;
        const uint8_t size[4] = {
            (uint8_t)length, (uint8_t)(length >> 8), (uint8_t)(length >> 16),
            (uint8_t)(length >> 24),
        };
        meta_append(&localized_icons, &tag_length, 1);
        meta_append(&localized_icons, tag, tag_length);
        meta_append(&localized_icons, size, sizeof(size));
        meta_append(&localized_icons, control_icon->data, length);
        ++icon_languages;
    }
    if (icon_languages && !localized_icons.failed)
        meta_tlv(&buffer, NUP_TLV_LOCALIZED_ICONS, localized_icons.data,
                 (uint32_t)localized_icons.size);
    free(localized_icons.data);

    log_line("nup-meta title=%016llX content=0x%08x nacp=%u icon=%zu version=%u languages=%d icons=%d size=%zu failed=%u",
             (unsigned long long)app->record.application_id, result,
             nacp_valid, icon_size, include_version && version_length,
             languages, icon_languages, buffer.size, buffer.failed);
    free_control_icons();
    if (buffer.failed || !buffer.size) {
        free(buffer.data);
        return false;
    }
    *out_meta = buffer.data;
    *out_size = buffer.size;
    return true;
}
