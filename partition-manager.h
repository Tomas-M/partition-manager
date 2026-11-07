#pragma once
#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <sys/stat.h>

typedef struct
{
    guint number, origin;
    guint64 start, size, fsbytes;
    char *type, *uuid, *name, *attrs, *fs, *fsuuid, *fslabel, *mount, *format;
    gboolean bootable, busy, readonly, grow;
} Partition;

typedef struct { guint64 start, size; } Region;
typedef struct { char *id, *name; } PartitionType;
typedef struct
{
    char *path, *title, *kind, *transport, *serial, *wwn, *majmin;
    char *label, *id, *snapshot, *warning;
    guint64 bytes, first, last, original_last;
    guint sector, table_length;
    gboolean readonly, busy, image, repair, unsupported, new_table;
    struct stat identity;
    GPtrArray *parts, *base, *types;
} Disk;

typedef struct
{
    const char *id, *name, *formatter, *grower;
    gboolean format_available, grow_available;
} Filesystem;
typedef void (*ApplyProgress)(const char *step, guint completed, guint total, gpointer data);
typedef struct { gboolean allow_busy, reboot_required; ApplyProgress progress; gpointer progress_data; } ApplyOptions;

extern Filesystem filesystems[];
extern const guint filesystem_count;

Partition *partition_new(void);
Partition *partition_copy(const Partition *part);
void partition_free(gpointer part);
Disk *disk_new(const char *path);
Disk *disk_copy(const Disk *disk);
void disk_free(gpointer disk);
GPtrArray *parts_copy(GPtrArray *parts);
Partition *find_part(GPtrArray *parts, guint number);
GArray *free_regions(const Disk *disk);
guint64 grow_limit(const Disk *disk, const Partition *part);
gboolean disk_changed(const Disk *disk);
gboolean layout_changed(const Disk *disk);
gboolean validate_plan(const Disk *disk, GError **error);
char *disk_script(const Disk *disk);
char *plan_description(const Disk *disk);
char *format_size(guint64 bytes);
const char *device_icon(const Disk *disk);
guint64 align_up(guint64 value, guint64 grain);
Filesystem *find_filesystem(const char *id);
const char *filesystem_type(const Disk *disk, const char *fs);
gboolean fail(GError **error, const char *format, ...) G_GNUC_PRINTF(2,3);

gboolean command_run(const char *const *argv, const char *input, int fd, char **output, char **diagnostic, GError **error);
gboolean engine_init(GError **error);
gboolean engine_new_table(Disk *disk, const char *label, GError **error);
guint64 capacity_last(const Disk *disk);
GPtrArray *engine_scan(const char *only, gboolean include_loop, GError **error);
Disk *engine_read(const char *path, GError **error);
Disk *engine_recheck(Disk *disk, GError **error);
gboolean engine_unmount(Disk *disk, GError **error);
gboolean engine_apply(Disk *disk, const char *backup_root, GString *log, ApplyOptions *options, GError **error);
gboolean engine_check(Disk *disk, char **output, GError **error);

int ui_run(int argc, char **argv, const char *only, gboolean readonly, gboolean include_loop);
