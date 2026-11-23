#include "app_internal.h"

/* Rendering and controller-driven screen transitions. */

#define COLOR_BACKGROUND UI_RGBA(14, 18, 27, 255)
#define COLOR_HEADER UI_RGBA(20, 25, 36, 255)
#define COLOR_PANEL UI_RGBA(25, 31, 44, 255)
#define COLOR_PANEL_LIGHT UI_RGBA(34, 41, 57, 255)
#define COLOR_ACCENT UI_RGBA(71, 190, 177, 255)
#define COLOR_ACCENT_DARK UI_RGBA(31, 85, 82, 255)
#define COLOR_TEXT UI_RGBA(241, 244, 249, 255)
#define COLOR_MUTED UI_RGBA(156, 166, 184, 255)
#define COLOR_SUBTLE UI_RGBA(91, 102, 122, 255)
#define COLOR_SUCCESS UI_RGBA(91, 208, 139, 255)
#define COLOR_WARNING UI_RGBA(245, 185, 74, 255)
#define COLOR_ERROR UI_RGBA(239, 96, 105, 255)

static void format_size(char *output, size_t size, u64 bytes);

static void draw_panel(int x, int y, int width, int height) {
    uiFillRoundedRect(x, y, width, height, 14, COLOR_PANEL);
}

static int draw_button_hint(int x, const char *button, const char *action) {
    int button_width = uiTextWidth(18, button) + 22;
    if (button_width < 34) button_width = 34;
    uiFillRoundedRect(x, 657, button_width, 34, 17, COLOR_PANEL_LIGHT);
    if (!strcmp(button, "-")) {
        uiFillRoundedRect(x + (button_width - 12) / 2, 673,
                          12, 2, 1, COLOR_TEXT);
    } else {
        uiDrawText(x + (button_width - uiTextWidth(18, button)) / 2,
                   663, 18, COLOR_TEXT, button);
    }
    x += button_width + 10;
    uiDrawText(x, 663, 18, COLOR_MUTED, action);
    return x + uiTextWidth(18, action) + 28;
}

static void draw_exit_hint(void) {
    const char *button = "+";
    const char *action = "Exit";
    int button_width = uiTextWidth(18, button) + 22;
    if (button_width < 34) button_width = 34;
    int total_width = button_width + 10 + uiTextWidth(18, action);
    draw_button_hint(UI_WIDTH - 54 - total_width, button, action);
}

static int draw_footer(const char *button1, const char *action1,
                       const char *button2, const char *action2,
                       const char *button3, const char *action3,
                       const char *button4, const char *action4) {
    uiFillRect(0, 640, UI_WIDTH, 80, COLOR_HEADER);
    int x = 54;
    if (button1) x = draw_button_hint(x, button1, action1);
    if (button2) x = draw_button_hint(x, button2, action2);
    if (button3) x = draw_button_hint(x, button3, action3);
    if (button4) x = draw_button_hint(x, button4, action4);
    return x;
}

static void draw_header(const char *section) {
    uiBeginFrame(COLOR_BACKGROUND);
    uiFillRect(0, 0, UI_WIDTH, 92, COLOR_HEADER);
    uiFillRect(0, 90, UI_WIDTH, 2, COLOR_ACCENT);

    uiDrawText(54, 20, 34, COLOR_TEXT, "KaiDump");
    uiDrawTextf(54, 57, 16, COLOR_MUTED, "v%s  /  %s",
                KAIDUMP_VERSION, section);

    const bool kaihen = hosversionIsKaiHEN();
    const bool kaihen_ready = kaihen && (!g.kaihen_checked || g.kaihen_ready);
    const char *hen_text = !kaihen
        ? "kaiHEN not detected"
        : kaihen_ready ? "kaiHEN connected" : "kaiHEN update required";
    int hen_width = uiTextWidth(17, hen_text) + 28;
    uiFillRoundedRect(1218 - hen_width, 18, hen_width, 32, 16,
                      kaihen_ready ? COLOR_ACCENT_DARK
                                   : UI_RGBA(89, 39, 47, 255));
    uiDrawText(1218 - hen_width + 14, 24, 17,
               kaihen_ready ? COLOR_SUCCESS : COLOR_ERROR, hen_text);
}

static void draw_empty_state(const char *title, const char *message,
                             uint32_t color) {
    draw_panel(260, 220, 760, 245);
    uiFillRoundedRect(592, 255, 96, 8, 4, color);
    int title_width = uiTextWidth(30, title);
    uiDrawText(640 - title_width / 2, 295, 30, COLOR_TEXT, title);
    int message_width = uiTextWidth(20, message);
    uiDrawText(640 - message_width / 2, 350, 20, COLOR_MUTED, message);
}

void draw_kaihen_error(const KaihenCompatibility *check) {
    const char *title = "kaiHEN update required";
    const char *message =
        "KaiDump needs kaiHEN 20261117b or newer (protocol 7).";
    const char *action =
        "Update kaiHEN, rerun its bootstrap, then launch KaiDump again.";

    if (check->reason == KAIHEN_NOT_DETECTED) {
        title = "kaiHEN was not detected";
        message = "KaiDump must be launched from hbmenu under kaiHEN.";
        action = "Start kaiHEN and hbmenu, then launch KaiDump again.";
    } else if (check->reason == KAIHEN_INITIALIZATION_FAILED) {
        title = "kaiHEN did not start correctly";
        message = "The installed version is new enough, but HEN initialization failed.";
        action = "Check KaiDump.log, then rerun the kaiHEN bootstrap.";
    } else if (check->reason == KAIHEN_LOADER_OVERRIDE_MISSING) {
        title = "ExeFS access is unavailable";
        message = "kaiHEN did not pass its fsp-ldr session to KaiDump.";
        action = "Update kaiHEN and rerun its bootstrap before trying again.";
    } else if (check->reason == KAIHEN_STATUS_UNAVAILABLE) {
        title = "Could not verify kaiHEN";
        message = "KaiDump could not read the current HEN protocol status.";
        action = "Update kaiHEN and rerun its bootstrap before trying again.";
    }

    draw_header("Compatibility check");
    draw_panel(178, 154, 924, 382);
    uiFillRoundedRect(606, 190, 68, 68, 34, UI_RGBA(89, 39, 47, 255));
    uiDrawText(629, 201, 32, COLOR_ERROR, "!");
    uiDrawText(640 - uiTextWidth(30, title) / 2, 286, 30,
               COLOR_TEXT, title);
    uiDrawText(640 - uiTextWidth(19, message) / 2, 347, 19,
               COLOR_MUTED, message);
    uiDrawText(640 - uiTextWidth(17, action) / 2, 385, 17,
               COLOR_MUTED, action);

    if (check->reason == KAIHEN_INITIALIZATION_FAILED) {
        char result[96];
        snprintf(result, sizeof(result), "Initialization result 0x%08x",
                 check->initialization_result);
        uiDrawText(640 - uiTextWidth(16, result) / 2, 443, 16,
                   COLOR_SUBTLE, result);
    } else if (check->protocol_version) {
        char protocol[96];
        snprintf(protocol, sizeof(protocol), "Detected protocol %u  /  required %u",
                 check->protocol_version, KAIDUMP_MIN_HEN_PROTOCOL);
        uiDrawText(640 - uiTextWidth(16, protocol) / 2, 443, 16,
                   COLOR_SUBTLE, protocol);
    }

    draw_footer(NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    draw_exit_hint();
    uiEndFrame();
}

void draw_loading(void) {
    draw_header("Starting");
    draw_empty_state("Loading titles",
                     "Reading the accessible software on this console...",
                     COLOR_ACCENT);
    uiEndFrame();
}

static void draw_titles(void) {
    load_visible_metadata();
    load_grid_icons();
    draw_header("Installed titles");

    if (R_FAILED(g.init_result) || R_FAILED(g.list_result)) {
        Result result = R_FAILED(g.init_result) ? g.init_result : g.list_result;
        draw_empty_state("Could not load titles",
                         "Press X to retry the title service.", COLOR_ERROR);
        uiDrawTextf(510, 405, 16, COLOR_SUBTLE,
                    "Result 0x%08x  /  module %u  /  description %u",
                    result, R_MODULE(result), R_DESCRIPTION(result));
    } else if (!g.count) {
        draw_empty_state("No accessible titles",
                         "Install a game or insert a game card, then refresh.",
                         COLOR_WARNING);
    } else {
        s32 page = g.selected / ROWS_PER_PAGE;
        s32 pages = (g.count + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
        s32 first = page * ROWS_PER_PAGE;
        s32 last = first + ROWS_PER_PAGE;
        if (last > g.count) last = g.count;

        uiDrawTextf(54, 108, 18, COLOR_MUTED, "%d accessible titles", g.count);
        char page_text[48];
        snprintf(page_text, sizeof(page_text), "Page %d of %d", page + 1, pages);
        uiDrawText(1226 - uiTextWidth(18, page_text), 108, 18,
                   COLOR_MUTED, page_text);
        draw_panel(54, 139, 1172, 474);

        if (g.title_grid) {
            for (s32 i = first; i < last; ++i) {
                Application *app = &g.applications[i];
                int slot = i - first;
                int column = slot % 5;
                int row = slot / 5;
                int x = 66 + column * 230;
                int y = 148 + row * 228;
                bool selected = i == g.selected;
                uiFillRoundedRect(x, y, 214, 216, 12,
                                  selected ? COLOR_PANEL_LIGHT
                                           : COLOR_BACKGROUND);
                if (selected)
                    uiFillRoundedRect(x, y + 211, 214, 5, 2, COLOR_ACCENT);

                if (g.grid_icon_loaded[slot]) {
                    uiDrawThumbnail(slot, x + 29, y + 10, 156, 156);
                } else {
                    uiFillRoundedRect(x + 57, y + 38, 100, 100, 22,
                                      COLOR_PANEL_LIGHT);
                    uiDrawText(x + 91, y + 61, 42, COLOR_SUBTLE, "?");
                }
                const char *name = app->title[0]
                    ? app->title : "Title unavailable";
                int name_width = uiTextWidth(15, name);
                int name_x = name_width < 190
                    ? x + (214 - name_width) / 2 : x + 12;
                uiDrawTextEllipsized(name_x, y + 177, 15,
                                     app->title[0] ? COLOR_TEXT : COLOR_MUTED,
                                     name, 190);
            }
        } else {
            for (s32 i = first; i < last; ++i) {
                Application *app = &g.applications[i];
                int row = i - first;
                int y = 148 + row * 46;
                bool selected = i == g.selected;
                if (selected) {
                    uiFillRoundedRect(66, y, 1148, 40, 9, COLOR_PANEL_LIGHT);
                    uiFillRoundedRect(66, y, 5, 40, 2, COLOR_ACCENT);
                }
                const char *name = app->title[0]
                    ? app->title : "Title unavailable";
                uiDrawTextEllipsized(88, y + 7, 21,
                                     app->title[0] ? COLOR_TEXT : COLOR_MUTED,
                                     name, 760);
                char id[24];
                snprintf(id, sizeof(id), "%016llX",
                         (unsigned long long)app->record.application_id);
                uiDrawText(1190 - uiTextWidth(17, id), y + 9, 17,
                           selected ? COLOR_ACCENT : COLOR_SUBTLE, id);
            }
        }
    }

    int footer_x = draw_footer("-",
                               g.title_grid ? "List view" : "Tile view",
                               "L / R", "Page", "D-Pad",
                               g.title_grid ? "Navigate" : "Scroll",
                               "A", "Select");
    draw_button_hint(footer_x, "X", "Refresh");
    draw_exit_hint();
    uiEndFrame();
}

int selected_content_count(void) {
    int count = 0;
    for (s32 i = 0; i < g.choice_count; ++i)
        if (g.choices[i].selected) ++count;
    return count;
}

static uint32_t content_type_color(ContentKind kind) {
    switch (kind) {
        case CONTENT_GAME: return COLOR_ACCENT;
        case CONTENT_BASE: return COLOR_ACCENT;
        case CONTENT_DLC: return UI_RGBA(191, 120, 242, 255);
        default: return COLOR_SUBTLE;
    }
}

static const char *content_choice_name(const ContentChoice *choice) {
    switch (choice->kind) {
        case CONTENT_GAME: return "Game";
        case CONTENT_BASE: return "Base game";
        case CONTENT_DLC: return "DLC";
        default: return "Content";
    }
}

static void draw_details(void) {
    Application *app = &g.applications[g.selected];
    draw_header("Title details");

    draw_panel(54, 112, 1172, 106);
    uiDrawTextEllipsized(78, 128, 31, COLOR_TEXT,
                         app->title[0] ? app->title : "Title unavailable", 850);
    uiDrawTextf(78, 174, 17, COLOR_MUTED, "Title ID  %016llX",
                (unsigned long long)app->record.application_id);
    if (app->version[0]) {
        char version[64];
        snprintf(version, sizeof(version), "Version  %s", app->version);
        uiDrawText(1198 - uiTextWidth(17, version), 174, 17,
                   COLOR_MUTED, version);
    }

    draw_panel(54, 235, 755, 378);
    uiDrawText(78, 254, 22, COLOR_TEXT, "Select content");
    uiDrawTextf(785 - uiTextWidth(16, "00 selected"), 259, 16,
                selected_content_count() ? COLOR_ACCENT : COLOR_ERROR,
                "%d selected", selected_content_count());
    if (R_FAILED(g.details_result)) {
        uiDrawTextf(78, 300, 18, COLOR_ERROR,
                    "Content metadata failed: 0x%08x", g.details_result);
    } else if (!g.choice_count) {
        uiDrawText(78, 300, 18, COLOR_MUTED,
                   "No exportable content was returned.");
    } else {
        const int rows = 5;
        int first = (g.content_cursor / rows) * rows;
        int last = first + rows;
        if (last > g.choice_count) last = g.choice_count;
        for (int i = first; i < last; ++i) {
            ContentChoice *choice = &g.choices[i];
            NsApplicationContentMetaStatus *meta = &g.meta[choice->meta_index];
            int y = 292 + (i - first) * 58;
            bool focused = i == g.content_cursor;
            if (focused)
                uiFillRoundedRect(68, y, 727, 51, 9, COLOR_PANEL_LIGHT);

            uint32_t color = content_type_color(choice->kind);
            uiFillRoundedRect(82, y + 11, 29, 29, 7,
                              choice->selected ? color : COLOR_BACKGROUND);
            if (choice->selected) {
                uiDrawLine(89, y + 26, 95, y + 32, 2, COLOR_BACKGROUND);
                uiDrawLine(95, y + 32, 105, y + 19, 2, COLOR_BACKGROUND);
            } else {
                uiStrokeRoundedRect(82, y + 11, 29, 29, 7, 2, COLOR_SUBTLE);
            }

            uiDrawText(128, y + 3, 19, COLOR_TEXT,
                       content_choice_name(choice));
            if (choice->kind == CONTENT_GAME && choice->update_index >= 0) {
                uiDrawText(128, y + 28, 14, COLOR_MUTED,
                           "Base + installed update");
            } else if (choice->kind == CONTENT_DLC &&
                       choice->update_index >= 0) {
                uiDrawTextf(128, y + 28, 14, COLOR_MUTED,
                            "DLC + update (v%u)  /  %s",
                            g.meta[choice->update_index].version,
                            storage_name(meta->storageID));
            } else if (choice->kind == CONTENT_BASE) {
                uiDrawText(128, y + 28, 14, COLOR_MUTED,
                           "Game card base");
            } else {
                uiDrawTextf(128, y + 28, 14, COLOR_MUTED,
                            "Version %u  /  %s", meta->version,
                            storage_name(meta->storageID));
            }
            uiDrawTextf(775 - uiTextWidth(14, "0000000000000000"), y + 17,
                        14, focused ? color : COLOR_SUBTLE, "%016llX",
                        (unsigned long long)meta->application_id);
        }
        if (g.choice_count > rows) {
            int page = g.content_cursor / rows + 1;
            int pages = (g.choice_count + rows - 1) / rows;
            uiDrawTextf(684, 583, 14, COLOR_SUBTLE,
                        "Page %d of %d", page, pages);
        }
    }

    draw_panel(829, 235, 397, 378);
    uiFillRoundedRect(853, 259, 349, 330, 14, COLOR_BACKGROUND);
    if (g.selected_icon_loaded) {
        uiDrawImage(865, 271, 325, 306);
    } else {
        uiFillRoundedRect(977, 374, 100, 100, 22, COLOR_PANEL_LIGHT);
        uiDrawText(1009, 399, 42, COLOR_SUBTLE, "?");
        uiDrawText(951, 500, 16, COLOR_MUTED, "Icon unavailable");
    }

    draw_footer("D-Pad", "Select", "A", "Select", "X", "Continue",
                "B", "Titles");
    draw_exit_hint();
    uiEndFrame();
}

static void draw_option_row(int y, const char *label, const char *value,
                            const char *description, bool focused,
                            uint32_t value_color, uint32_t description_color) {
    if (focused)
        uiFillRoundedRect(250, y, 780, 94, 12, COLOR_PANEL_LIGHT);
    uiDrawText(278, y + 14, 21, COLOR_TEXT, label);
    uiDrawText(1000 - uiTextWidth(20, value), y + 15, 20,
               value_color ? value_color
                           : focused ? COLOR_ACCENT : COLOR_MUTED,
               value);
    uiDrawText(278, y + 51, 16,
               description_color ? description_color : COLOR_MUTED,
               description);
}

static const char *output_format_name(void) {
    switch (g.output_format) {
        case OUTPUT_TAR: return "TAR archive";
        case OUTPUT_NUP: return "NUP archive (Slowest)";
        default: return "Extracted files";
    }
}

static const char *output_format_short_name(void) {
    switch (g.output_format) {
        case OUTPUT_TAR: return "TAR";
        case OUTPUT_NUP: return "NUP";
        default: return "EXTRACTED";
    }
}

static void draw_output(void) {
    Application *app = &g.applications[g.selected];
    const char *destination = g.usb_enabled ? "USB" : g.ftp_enabled ? "FTP" : "SD card";
    const char *destination_description;
    if (g.usb_enabled) {
        destination_description = g.output_format == OUTPUT_EXTRACTED
            ? "Browse extracted files over USB MTP."
            : g.output_format == OUTPUT_TAR
                ? "Stream a virtual TAR over USB MTP."
                : "Stream a virtual NUP over USB MTP.";
    } else if (g.ftp_enabled) {
        destination_description = !g.ftp_available
            ? "No LAN network detected. FTP requires LAN."
            : g.output_format == OUTPUT_EXTRACTED
                ? "Browse extracted files over FTP."
                : g.output_format == OUTPUT_TAR
                    ? "Stream a virtual TAR over FTP."
                    : "Hash the content, then stream a virtual NUP to FTP.";
    } else {
        destination_description = "Export output to the SD Card.";
    }
    draw_header("Output options");
    draw_panel(218, 132, 844, 472);
    uiDrawTextEllipsized(250, 158, 25, COLOR_TEXT,
                         app->title[0] ? app->title : "Title unavailable", 760);
    uiDrawTextf(250, 198, 16, COLOR_MUTED, "%d content item%s selected",
                selected_content_count(), selected_content_count() == 1 ? "" : "s");

    const char *format_description = g.output_format == OUTPUT_EXTRACTED
        ? "Copy ExeFS and RomFS as regular directories."
        : g.output_format == OUTPUT_TAR
            ? "One uncompressed archive, prepared without hashing content."
            : "One unencrypted NX package containing the selected content.";
    draw_option_row(250, "Format", output_format_name(), format_description,
                    g.output_cursor == 0, 0, 0);
    bool ftp_unavailable = g.ftp_enabled && !g.ftp_available;
    draw_option_row(360, "Destination", destination,
                    destination_description,
                    g.output_cursor == 1,
                    ftp_unavailable ? COLOR_ERROR : 0,
                    ftp_unavailable ? COLOR_ERROR : 0);
    if (g.output_format == OUTPUT_NUP && (g.ftp_enabled || g.usb_enabled))
        uiDrawText(278, 482, 15, COLOR_MUTED,
                   "Virtual output requires one hashing pass before transfer begins.");
    else if (g.output_format == OUTPUT_TAR && (g.ftp_enabled || g.usb_enabled))
        uiDrawText(278, 482, 15, COLOR_MUTED,
                   "TAR is available after a quick file inventory; no hash pass needed.");
    else if (g.output_format == OUTPUT_EXTRACTED &&
             (g.ftp_enabled || g.usb_enabled))
        uiDrawText(278, 482, 15, COLOR_MUTED,
                   "Files are read directly from the selected game content.");

    draw_footer("D-Pad", "Select", "A / Left / Right", "Change",
                "X", "Continue", "B", "Back");
    draw_exit_hint();
    uiEndFrame();
}

static void draw_confirm(void) {
    Application *app = &g.applications[g.selected];
    draw_header("Confirm dump");
    draw_panel(218, 137, 844, 462);
    uiFillRoundedRect(250, 169, 56, 56, 28, UI_RGBA(83, 67, 34, 255));
    uiDrawText(270, 179, 28, COLOR_WARNING, "!");
    uiDrawText(330, 164, 28, COLOR_TEXT, "Start this export?");
    uiDrawTextEllipsized(330, 201, 18, COLOR_MUTED,
                         app->title[0] ? app->title : "Title unavailable", 680);

    uiDrawTextf(250, 256, 16, COLOR_SUBTLE,
                "CONTENT  /  %d SELECTED  /  %s",
                selected_content_count(), output_format_short_name());
    uiDrawText(250, 286, 16, COLOR_SUBTLE, "DESTINATION");
    uiFillRoundedRect(250, 314, 780, 58, 9, COLOR_BACKGROUND);
    char output_name[256];
    char output_path[640];
    if (g.ftp_enabled)
        uiDrawText(270, 331, 17, COLOR_MUTED,
                   g.output_format == OUTPUT_EXTRACTED
                       ? "Extracted files over FTP; no dump data is written to SD"
                       : g.output_format == OUTPUT_TAR
                           ? "Virtual read-only TAR over FTP; no dump data is written to SD"
                           : "Virtual read-only NUP over FTP; no dump data is written to SD");
    else if (g.usb_enabled)
        uiDrawText(270, 331, 17, COLOR_MUTED,
                   g.output_format == OUTPUT_EXTRACTED
                       ? "Extracted files over USB; no dump data is written to SD"
                       : g.output_format == OUTPUT_TAR
                           ? "Virtual read-only TAR over USB; no dump data is written to SD"
                           : "Virtual read-only NUP over USB; no dump data is written to SD");
    else {
        build_output_name(app, "", output_name, sizeof(output_name));
        if (g.output_format == OUTPUT_EXTRACTED) {
            snprintf(output_path, sizeof(output_path),
                     "sdmc:/switch/KaiDump/dumps/%s/", output_name);
        } else {
            char archive_name[256];
            build_output_name(app,
                              g.output_format == OUTPUT_NUP ? ".nup" : ".tar",
                              archive_name, sizeof(archive_name));
            snprintf(output_path, sizeof(output_path),
                     "sdmc:/switch/KaiDump/dumps/%s/%s",
                     output_name, archive_name);
        }
        uiDrawTextEllipsized(270, 331, 17, COLOR_MUTED,
                             output_path, 736);
    }

    uiDrawText(250, 400, 19, COLOR_TEXT,
               "Existing files with matching names will be replaced.");
    uiDrawText(250, 434, 18, COLOR_MUTED,
               "KaiDump never writes to game content or NAND.");
    uiDrawText(250, 464, 18, COLOR_MUTED,
               g.ftp_enabled
                   ? "The selected content remains mounted while FTP is running."
                   : g.usb_enabled
                       ? "The selected content remains mounted while USB is running."
                   : "Make sure the SD card has enough free space.");

    uiFillRoundedRect(250, 514, 512, 54, 27, COLOR_ACCENT_DARK);
    uiDrawText(274, 530, 18, COLOR_SUCCESS,
               "Press A to begin");

    draw_footer("A", "Start export", "B", "Cancel", NULL, NULL, NULL, NULL);
    draw_exit_hint();
    uiEndFrame();
}

void draw_ftp(void) {
    draw_header("FTP server");
    draw_panel(218, 145, 844, 430);
    uiFillRoundedRect(250, 180, 112, 10, 5, COLOR_ACCENT);
    uiDrawText(250, 220, 30, COLOR_TEXT, "Read-only FTP is running");
    uiDrawText(250, 270, 16, COLOR_MUTED, "CONNECT TO");
    uiFillRoundedRect(250, 300, 780, 62, 10, COLOR_BACKGROUND);
    uiDrawTextEllipsized(272, 320, 18, COLOR_SUCCESS,
                         g.ftp_address[0] ? g.ftp_address : "Starting network...", 736);
    uiDrawText(250, 400, 17, COLOR_MUTED,
               g.status[0] ? g.status : "Waiting for an FTP client");
    char bytes[48];
    format_size(bytes, sizeof(bytes), g.copied_bytes);
    uiDrawTextf(250, 444, 17, COLOR_SUBTLE, "Transferred  %s", bytes);
    uiDrawText(250, 505, 16, COLOR_SUBTLE,
               "Anonymous login is accepted. Upload and delete commands are disabled.");
    uiDrawText(250, 531, 16, COLOR_SUBTLE,
               "Set your FTP client to binary transfers; ASCII mode downloads are refused.");
    draw_footer("B", "Stop server", NULL, NULL, NULL, NULL, NULL, NULL);
    uiEndFrame();
}

void draw_usb(void) {
    app_status_t status;
    status_snapshot(&status);
    draw_header("USB transfer");
    draw_panel(218, 145, 844, 430);
    uiFillRoundedRect(250, 180, 112, 10, 5,
                      status.state == ST_ERROR ? COLOR_ERROR : COLOR_ACCENT);

    const char *heading = "Waiting for a USB connection";
    if (status.state == ST_CONNECTED) heading = "USB connected";
    else if (status.state == ST_SESSION)
        heading = g.output_format == OUTPUT_NUP ? "NUP ready on USB"
            : g.output_format == OUTPUT_TAR ? "TAR ready on USB"
            : "Extracted files ready on USB";
    else if (status.state == ST_TRANSFER)
        heading = g.output_format == OUTPUT_NUP ? "Transferring NUP over USB"
            : g.output_format == OUTPUT_TAR ? "Transferring TAR over USB"
            : "Transferring file over USB";
    else if (status.state == ST_ERROR) heading = "USB could not start";
    uiDrawText(250, 220, 30, COLOR_TEXT, heading);

    uiDrawText(250, 270, 16, COLOR_MUTED, "DEVICE");
    uiFillRoundedRect(250, 300, 780, 62, 10, COLOR_BACKGROUND);
    const char *usb_hint = g.output_format == OUTPUT_NUP
        ? "KaiDump — open the NUP file from your computer"
        : g.output_format == OUTPUT_TAR
            ? "KaiDump — open the TAR file from your computer"
            : "KaiDump — browse extracted files from your computer";
    uiDrawText(272, 320, 18, COLOR_SUCCESS, usb_hint);

    if (status.state == ST_ERROR) {
        uiDrawTextEllipsized(250, 400, 17, COLOR_ERROR,
                             status.error, 760);
    } else if (status.usb_speed_mps >= 1024) {
        uiDrawText(250, 400, 17, COLOR_MUTED, "Link: USB 3 SuperSpeed");
    } else if (status.usb_speed_mps >= 512) {
        uiDrawText(250, 400, 17, COLOR_MUTED, "Link: USB 2 High-Speed");
    } else {
        uiDrawText(250, 400, 17, COLOR_MUTED,
                   "Connect a USB-C data cable to the computer");
    }

    char bytes[48];
    format_size(bytes, sizeof(bytes), g.copied_bytes);
    uiDrawTextf(250, 444, 17, COLOR_SUBTLE, "Transferred  %s", bytes);
    uiDrawText(250, 505, 16, COLOR_SUBTLE,
               "The MTP device is read-only. No dump data is written to SD.");
    draw_footer("B", "Stop USB", NULL, NULL, NULL, NULL, NULL, NULL);
    uiEndFrame();
}

static void format_size(char *output, size_t size, u64 bytes) {
    static const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = (double)bytes;
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (!unit) snprintf(output, size, "%llu B", (unsigned long long)bytes);
    else snprintf(output, size, "%.2f %s", value, units[unit]);
}

static void draw_result(void) {
    draw_header("Dump result");
    bool succeeded = R_SUCCEEDED(g.dump_result);
    bool cancelled = R_MODULE(g.dump_result) == Module_Libnx &&
                     R_DESCRIPTION(g.dump_result) == KAIDUMP_ERROR_CANCELED;
    uint32_t status_color = succeeded ? COLOR_SUCCESS
                                      : (cancelled ? COLOR_WARNING : COLOR_ERROR);
    draw_panel(218, 150, 844, 418);
    uiFillRoundedRect(250, 185, 112, 10, 5, status_color);
    uiDrawText(250, 225, 31, COLOR_TEXT, succeeded ? "Dump completed"
               : (cancelled ? "Dump cancelled" : "Dump did not complete"));
    if (g.status[0])
        uiDrawTextEllipsized(250, 272, 19, COLOR_MUTED, g.status, 760);

    char bytes[48];
    format_size(bytes, sizeof(bytes), g.copied_bytes);
    uiFillRoundedRect(250, 330, 370, 106, 12, COLOR_PANEL_LIGHT);
    uiDrawText(274, 350, 16, COLOR_MUTED,
               g.progress_files_label[0]
                   ? g.progress_files_label : "FILES COPIED");
    uiDrawTextf(274, 382, 29, COLOR_TEXT, "%llu",
                (unsigned long long)g.copied_files);
    uiFillRoundedRect(650, 330, 380, 106, 12, COLOR_PANEL_LIGHT);
    uiDrawText(674, 350, 16, COLOR_MUTED,
               g.progress_bytes_label[0]
                   ? g.progress_bytes_label : "DATA COPIED");
    uiDrawText(674, 382, 29, COLOR_TEXT, bytes);

    if (!succeeded)
        uiDrawTextf(250, 482, 16, COLOR_SUBTLE,
                    "Result 0x%08x  /  module %u  /  description %u",
                    g.dump_result, R_MODULE(g.dump_result),
                    R_DESCRIPTION(g.dump_result));

    draw_footer("A / B", "Back to details", NULL, NULL, NULL, NULL,
                NULL, NULL);
    draw_exit_hint();
    uiEndFrame();
}

void draw(void) {
    switch (g.screen) {
        case SCREEN_TITLES: draw_titles(); break;
        case SCREEN_DETAILS: draw_details(); break;
        case SCREEN_OUTPUT: draw_output(); break;
        case SCREEN_CONFIRM: draw_confirm(); break;
        case SCREEN_FTP: draw_ftp(); break;
        case SCREEN_USB: draw_usb(); break;
        case SCREEN_RESULT: draw_result(); break;
    }
}

/*
 * Derive button presses from the held state instead of relying solely on
 * padGetButtonsDown().  This keeps a held button from being reported again
 * when the active controller or input style changes, which can otherwise
 * carry one press through several screens while docked.
 */
u64 buttons_pressed(PadState *pad) {
    static u64 previous = 0;
    u64 held = padGetButtons(pad);
    u64 pressed = held & ~previous;
    previous = held;
    return pressed;
}

void progress_set_labels(const char *files, const char *bytes) {
    snprintf(g.progress_files_label, sizeof(g.progress_files_label), "%s",
             files && files[0] ? files : "FILES COPIED");
    snprintf(g.progress_bytes_label, sizeof(g.progress_bytes_label), "%s",
             bytes && bytes[0] ? bytes : "DATA COPIED");
}

void progress_begin(const char *detail) {
    g.progress_total_bytes = 0;
    g.progress_started_tick = armGetSystemTick();
    g.progress_last_frame_tick = 0;
    g.progress_detail[0] = 0;
    progress_set_labels(NULL, NULL);
    g.progress_active = true;
    draw_progress(detail);
}

void progress_tick(const char *detail) {
    if (!g.progress_active || g.screen != SCREEN_RESULT) return;
    if (detail && detail[0] && detail != g.progress_detail)
        snprintf(g.progress_detail, sizeof(g.progress_detail), "%s", detail);
    u64 now = armGetSystemTick();
    if (g.progress_last_frame_tick &&
        armTicksToNs(now - g.progress_last_frame_tick) < 16000000ULL)
        return;
    draw_progress(NULL);
}

bool dump_cancelled(PadState *pad) {
    progress_tick(NULL);
    if (!appletMainLoop()) return true;
    uiPumpEvents();
    padUpdate(pad);
    return (buttons_pressed(pad) & HidNpadButton_B) != 0;
}

int ensure_directory(const char *path) {
    char work[512];
    size_t length = strlen(path);
    if (length >= sizeof(work)) return ENAMETOOLONG;
    memcpy(work, path, length + 1);
    for (char *p = work + strlen("sdmc:/"); *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(work, 0777) != 0 && errno != EEXIST) return errno;
        *p = '/';
    }
    if (mkdir(work, 0777) != 0 && errno != EEXIST) return errno;
    return 0;
}

void draw_progress(const char *relative) {
    if (relative && relative[0] && relative != g.progress_detail)
        snprintf(g.progress_detail, sizeof(g.progress_detail), "%s", relative);
    u64 now = armGetSystemTick();
    if (!g.progress_started_tick) g.progress_started_tick = now;
    g.progress_last_frame_tick = now;
    u64 elapsed_ms = armTicksToNs(now - g.progress_started_tick) / 1000000ULL;

    draw_header("Dumping title contents");
    Application *app = &g.applications[g.selected];
    draw_panel(160, 140, 960, 424);
    uiDrawText(198, 176, 28, COLOR_TEXT, "Copying title contents");
    uiDrawTextEllipsized(198, 216, 18, COLOR_MUTED,
                         app->title[0] ? app->title : "Title unavailable", 820);

    char bytes[48];
    format_size(bytes, sizeof(bytes), g.copied_bytes);
    uiFillRoundedRect(198, 278, 410, 102, 12, COLOR_PANEL_LIGHT);
    uiDrawText(222, 298, 16, COLOR_MUTED,
               g.progress_files_label[0]
                   ? g.progress_files_label : "FILES COPIED");
    uiDrawTextf(222, 330, 28, COLOR_TEXT, "%llu",
                (unsigned long long)g.copied_files);
    uiFillRoundedRect(628, 278, 454, 102, 12, COLOR_PANEL_LIGHT);
    uiDrawText(652, 298, 16, COLOR_MUTED,
               g.progress_bytes_label[0]
                   ? g.progress_bytes_label : "DATA COPIED");
    uiDrawText(652, 330, 28, COLOR_TEXT, bytes);

    const int bar_width = 884;
    uiFillRoundedRect(198, 414, bar_width, 10, 5, COLOR_PANEL_LIGHT);
    if (g.progress_total_bytes) {
        double fraction = (double)g.copied_bytes /
                          (double)g.progress_total_bytes;
        if (fraction > 1.0) fraction = 1.0;
        int fill = (int)(fraction * bar_width);
        if (fill > 0)
            uiFillRoundedRect(198, 414, fill, 10, 5, COLOR_ACCENT);
        int percent = (int)(fraction * 100.0);
        char percent_text[16];
        snprintf(percent_text, sizeof(percent_text), "%d%%", percent);
        uiDrawText(1082 - uiTextWidth(15, percent_text), 390, 15,
                   COLOR_MUTED, percent_text);

        if (fill > 12) {
            /* Keep the sweep stable as the completed width grows. */
            int phase = (int)((elapsed_ms / 3) % (u64)(bar_width + 60)) - 60;
            int start = phase < 0 ? 0 : phase;
            int end = phase + 60;
            if (end > fill) end = fill;
            if (end > start)
                uiFillRoundedRect(198 + start, 414, end - start, 10, 5,
                                  UI_RGBA(173, 239, 230, 150));
        }
    } else {
        const int activity_width = 150;
        const int travel = bar_width - activity_width;
        int phase = (int)((elapsed_ms / 2) % (u64)(travel * 2));
        int activity = phase <= travel ? phase : travel * 2 - phase;
        uiFillRoundedRect(198 + activity, 414, activity_width, 10, 5,
                          COLOR_ACCENT);
        uiDrawText(1082 - uiTextWidth(15, "Preparing"), 390, 15,
                   COLOR_MUTED, "Preparing");
    }
    uiDrawTextEllipsized(198, 451, 17, COLOR_MUTED,
                         g.progress_detail[0] ? g.progress_detail : "Preparing...",
                         884);
    uiDrawText(198, 508, 16, COLOR_SUBTLE,
               "Press B to cancel. Partial output will be kept.");
    draw_footer("B", "Cancel dump", NULL, NULL, NULL, NULL, NULL, NULL);
    uiEndFrame();
}

bool handle_input(u64 down, PadState *pad) {
    if (down & HidNpadButton_Plus) return false;

    if (g.screen == SCREEN_TITLES) {
        if (down & HidNpadButton_Minus) {
            g.title_grid = !g.title_grid;
        } else if (down & HidNpadButton_X) {
            refresh_applications();
        } else if ((down & HidNpadButton_A) && g.count > 0) {
            load_details();
        } else if (g.count > 0) {
            s32 old = g.selected;
            if (g.title_grid) {
                s32 first = (g.selected / ROWS_PER_PAGE) * ROWS_PER_PAGE;
                s32 last = first + ROWS_PER_PAGE;
                if (last > g.count) last = g.count;
                if ((down & HidNpadButton_AnyLeft) && g.selected > first)
                    --g.selected;
                if ((down & HidNpadButton_AnyRight) && g.selected + 1 < last)
                    ++g.selected;
                if ((down & HidNpadButton_AnyUp) &&
                    g.selected - 5 >= first)
                    g.selected -= 5;
                if ((down & HidNpadButton_AnyDown) &&
                    g.selected + 5 < last)
                    g.selected += 5;
            } else {
                if (down & HidNpadButton_AnyUp) --g.selected;
                if (down & HidNpadButton_AnyDown) ++g.selected;
            }
            if (down & HidNpadButton_L) g.selected -= ROWS_PER_PAGE;
            if (down & HidNpadButton_R) g.selected += ROWS_PER_PAGE;
            if (g.selected < 0) g.selected = 0;
            if (g.selected >= g.count) g.selected = g.count - 1;
            if (old == g.selected) return true;
        }
    } else if (g.screen == SCREEN_DETAILS) {
        if (down & HidNpadButton_B) {
            uiClearImage();
            g.selected_icon_loaded = false;
            g.screen = SCREEN_TITLES;
        } else if ((down & HidNpadButton_A) && g.choice_count > 0) {
            g.choices[g.content_cursor].selected =
                !g.choices[g.content_cursor].selected;
        } else if ((down & HidNpadButton_X) && selected_content_count() > 0) {
            g.output_cursor = 0;
            refresh_ftp_availability();
            g.screen = SCREEN_OUTPUT;
        } else if (g.choice_count > 0) {
            s32 old = g.content_cursor;
            if (down & HidNpadButton_AnyUp) --g.content_cursor;
            if (down & HidNpadButton_AnyDown) ++g.content_cursor;
            if (down & HidNpadButton_L) g.content_cursor -= 5;
            if (down & HidNpadButton_R) g.content_cursor += 5;
            if (g.content_cursor < 0) g.content_cursor = 0;
            if (g.content_cursor >= g.choice_count)
                g.content_cursor = g.choice_count - 1;
            if (old == g.content_cursor) return true;
        } else {
            return true;
        }
    } else if (g.screen == SCREEN_OUTPUT) {
        if (down & HidNpadButton_B) {
            g.screen = SCREEN_DETAILS;
        } else if (down & HidNpadButton_X) {
            if (g.ftp_enabled) {
                refresh_ftp_availability();
                if (g.ftp_available) g.screen = SCREEN_CONFIRM;
            } else {
                g.screen = SCREEN_CONFIRM;
            }
        } else if (down & HidNpadButton_AnyUp) {
            g.output_cursor = 0;
        } else if (down & HidNpadButton_AnyDown) {
            g.output_cursor = 1;
        } else if (down & (HidNpadButton_A | HidNpadButton_AnyLeft |
                           HidNpadButton_AnyRight)) {
            if (g.output_cursor == 0) {
                if (down & HidNpadButton_AnyLeft)
                    g.output_format = (g.output_format + OUTPUT_FORMAT_COUNT - 1) %
                                      OUTPUT_FORMAT_COUNT;
                else
                    g.output_format = (g.output_format + 1) % OUTPUT_FORMAT_COUNT;
            } else {
                if (!g.ftp_enabled && !g.usb_enabled) {
                    refresh_ftp_availability();
                    g.ftp_enabled = true;
                } else if (g.ftp_enabled) {
                    g.ftp_enabled = false;
                    g.usb_enabled = true;
                } else {
                    g.usb_enabled = false;
                }
            }
        } else return true;
    } else if (g.screen == SCREEN_CONFIRM) {
        if (down & HidNpadButton_B) g.screen = SCREEN_OUTPUT;
        else if (down & HidNpadButton_A) {
            dump_title(pad);
        } else return true;
    } else {
        if (down & (HidNpadButton_A | HidNpadButton_B)) g.screen = SCREEN_DETAILS;
        else return true;
    }
    draw();
    return true;
}
