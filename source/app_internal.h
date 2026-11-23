#ifndef KAIDUMP_APP_INTERNAL_H
#define KAIDUMP_APP_INTERNAL_H

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <zlib.h>

#include <switch.h>
#include <kaihen.h>

#include "ftp.h"
#include "mtp.h"
#include "nup.h"
#include "source_io.h"
#include "status.h"
#include "tar.h"
#include "transport.h"
#include "ui.h"
#include "virtual_file.h"
#include "virtual_tree.h"

#define KAIDUMP_VERSION "1.0.5"
#define MAX_APPLICATIONS 512
#define MAX_META_STATUS 64
#define ROWS_PER_PAGE 10
#define TITLE_CHARS 512
#define COPY_BUFFER_SIZE (1024 * 1024)
#define KAIDUMP_ERROR_CANCELED 200
#define KAIDUMP_ERROR_FAT32_FILE_LIMIT 202
#define KAIDUMP_MIN_HEN_PROTOCOL 7

_Static_assert(sizeof(FsCodeInfo) == 0x1a4,
               "KaiDump requires current Switch 2 libounce fsp-ldr support");

typedef enum {
    SCREEN_TITLES,
    SCREEN_DETAILS,
    SCREEN_OUTPUT,
    SCREEN_CONFIRM,
    SCREEN_FTP,
    SCREEN_USB,
    SCREEN_RESULT,
} Screen;

typedef enum {
    CONTENT_GAME,
    CONTENT_BASE,
    CONTENT_DLC,
} ContentKind;

typedef enum {
    OUTPUT_EXTRACTED,
    OUTPUT_TAR,
    OUTPUT_NUP,
    OUTPUT_FORMAT_COUNT,
} OutputFormat;

typedef enum {
    KAIHEN_COMPATIBLE,
    KAIHEN_NOT_DETECTED,
    KAIHEN_STATUS_UNAVAILABLE,
    KAIHEN_INITIALIZATION_FAILED,
    KAIHEN_PROTOCOL_TOO_OLD,
    KAIHEN_LOADER_OVERRIDE_MISSING,
} KaihenCompatibilityReason;

typedef struct {
    KaihenCompatibilityReason reason;
    Result ro_result;
    Result session_result;
    Result status_result;
    Result initialization_result;
    u32 protocol_version;
    bool marker_present;
    bool loader_override_present;
} KaihenCompatibility;

typedef struct {
    ContentKind kind;
    s32 meta_index;
    s32 update_index;
    bool selected;
} ContentChoice;

typedef struct {
    NsApplicationRecord record;
    char title[TITLE_CHARS];
    char version[0x10];
    u8 base_storage;
    u32 base_version;
    Result metadata_result;
    bool metadata_loaded;
} Application;

typedef struct {
    Application applications[MAX_APPLICATIONS];
    NsApplicationControlData control_data;
    NsApplicationContentMetaStatus meta[MAX_META_STATUS];
    s32 count;
    s32 selected;
    s32 meta_count;
    ContentChoice choices[MAX_META_STATUS];
    s32 choice_count;
    s32 content_cursor;
    s32 output_cursor;
    Result init_result;
    Result list_result;
    Result title_list_result;
    Result details_result;
    Result dump_result;
    bool ns_active;
    bool selected_icon_loaded;
    bool title_grid;
    bool grid_icon_loaded[UI_THUMBNAIL_COUNT];
    s32 grid_icon_first;
    OutputFormat output_format;
    bool ftp_enabled;
    bool usb_enabled;
    bool ftp_available;
    Result ftp_network_result;
    u8 *control_icon;
    size_t control_icon_size;
    Screen screen;
    u64 copied_bytes;
    u64 copied_files;
    u64 progress_total_bytes;
    u64 progress_started_tick;
    u64 progress_last_frame_tick;
    char progress_detail[768];
    char progress_files_label[32];
    char progress_bytes_label[32];
    bool progress_active;
    char status[160];
    char ftp_address[256];
    bool log_active;
    bool kaihen_checked;
    bool kaihen_ready;
    bool romfs_base_only;
    bool exefs_base_only;
    bool romfs_self_matched;
    /* A selected DLC has an installed update that could not be merged. */
    bool dlc_update_missing;
} AppState;

extern AppState g;
extern unsigned char *g_copy_buffer;
extern FILE *g_log;
extern Service g_license_context;
extern Service g_license_application_context;
extern u64 g_exefs_attempt;
extern volatile int g_usb_stop;
extern Thread g_usb_thread;

/* Runtime and logging. */
void refresh_ftp_availability(void);
size_t load_control_icon(void);
void log_line(const char *format, ...);
const char *result_name(Result result);
void log_result(const char *operation, Result result);
void format_result_code(char *out, size_t out_size, Result result);
void log_process_identity(void);
const char *kaihen_reason_name(KaihenCompatibilityReason reason);
KaihenCompatibility check_kaihen_compatibility(void);
void close_ns(void);

/* Title catalog. */
void build_output_name(const Application *app, const char *extension,
                       char *output, size_t output_size);
void copy_display_text(char *out, size_t out_size, const char *input);
void refresh_applications(void);
void load_visible_metadata(void);
const char *storage_name(u8 id);
void load_grid_icons(void);
void load_details(void);
Result load_control_from_content(Application *app);
void title_from_nacp(Application *app, NacpStruct *nacp);

/* Screens, input, and progress. */
void draw_kaihen_error(const KaihenCompatibility *check);
void draw_loading(void);
void draw_ftp(void);
void draw_usb(void);
void draw(void);
u64 buttons_pressed(PadState *pad);
int selected_content_count(void);
void progress_begin(const char *detail);
void progress_set_labels(const char *files, const char *bytes);
void progress_tick(const char *detail);
bool dump_cancelled(PadState *pad);
int ensure_directory(const char *path);
void draw_progress(const char *relative);
bool handle_input(u64 down, PadState *pad);

/* Content access and extracted export. */
Result open_title_exefs(FsFileSystem *out, u64 application_id,
                        const NsApplicationContentMetaStatus *status,
                        const NsApplicationContentMetaStatus *original_status);
Result open_data_storage_by_path(FsStorage *out, const char *path,
                                 FsContentAttributes attributes,
                                 u32 proxy_type);
Result resolve_installed_content(
    const NsApplicationContentMetaStatus *status, u8 content_type,
    char *path, size_t path_size, FsContentAttributes *attributes);
void close_code_filesystem(FsFileSystem *filesystem);
Result mount_title_romfs(u64 application_id, bool *romfs_device);
/* The base RomFS without the update (sets g.romfs_base_only when an update
   is installed), so it matches a base ExeFS. */
Result mount_base_title_romfs(u64 application_id, bool *romfs_device);
/* Keeps the more informative of two RomFS open failures (e.g. a missing
   update key over a generic InvalidArgument from a later fallback). */
Result prefer_romfs_failure(Result kept, Result candidate);
void unmount_title_romfs(const char *name, bool romfs_device);
const NsApplicationContentMetaStatus *find_program_status(u8 meta_type);
/* The installed update (DataPatch) of a DLC, or NULL. */
const NsApplicationContentMetaStatus *find_dlc_update(
    const NsApplicationContentMetaStatus *dlc);
/* The DLC with its update applied, verified by size; fails (and the caller
   falls back to the plain DLC) when FS does not merge them. */
Result open_patched_addon_storage(const NsApplicationContentMetaStatus *dlc,
                                  const NsApplicationContentMetaStatus *patch,
                                  FsStorage *out);
/* Programs of titles with more than one (Switch 2 Editions). */
#define PROGRAM_INDEX_AUTO 0xff
void set_program_index_override(int program_index);
s32 game_program_indices(u8 *out, s32 capacity);
/* Serialized NUP application meta TLVs (name, icon, ...) for the selected
   title; include_version adds the NACP and display version. */
bool build_nup_app_meta(bool include_version, uint8_t **out_meta,
                        size_t *out_size);

/* Every icon_<Language>.dat of the control data, read only while
   g_capture_control_icons is set (the grid loads just one icon). */
#define MAX_CONTROL_ICONS 32
typedef struct {
    char language[48];
    u8 *data;
    size_t size;
} ControlIcon;
extern bool g_capture_control_icons;
extern ControlIcon g_control_icons[MAX_CONTROL_ICONS];
extern int g_control_icon_count;
void free_control_icons(void);
Result export_selected_content(const NsApplicationContentMetaStatus *status,
                               const char *root, PadState *pad);
Result export_combined_game(u64 application_id, const char *root,
                            PadState *pad);

/* Archive and transfer orchestration. */
void dump_title(PadState *pad);

#endif
