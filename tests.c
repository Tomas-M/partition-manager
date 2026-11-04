#define _GNU_SOURCE
#include "partition-manager.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

/* Unit tests use an ordinary four-extent disk with no host device access. */
static Disk *sample(void)
{
    Disk *disk=disk_new("/dev/example");
    disk->bytes=512*10000; disk->first=100; disk->last=9999;
    g_free(disk->label); disk->label=g_strdup("gpt");
    for (guint i=1; i<=3; i++)
    {
        Partition *part=partition_new(); part->number=i; part->origin=i; part->start=100+(i-1)*1000; part->size=1000;
        g_ptr_array_add(disk->parts,part);
    }
    g_ptr_array_unref(disk->base); disk->base=parts_copy(disk->parts);
    return disk;
}

/* Removing adjacent extents exposes one derived free interval, including padding. */
static void test_merge(void)
{
    Disk *disk=sample(); g_ptr_array_remove_index(disk->parts,1);
    GArray *free=free_regions(disk); g_assert_cmpuint(free->len,==,2); g_array_unref(free);
    g_ptr_array_remove_index(disk->parts,1);
    free=free_regions(disk); g_assert_cmpuint(free->len,==,1);
    Region region=g_array_index(free,Region,0); g_assert_cmpuint(region.start,==,1100); g_assert_cmpuint(region.size,==,8900);
    g_array_unref(free); disk_free(disk);
}

/* Rejected moves, shrinking and overlaps never reach the command layer. */
static void test_validation(void)
{
    Disk *disk=sample(); GError *error=NULL;
    g_assert_true(validate_plan(disk,&error));
    Partition *part=disk->parts->pdata[1]; part->start++;
    g_assert_false(validate_plan(disk,&error)); g_clear_error(&error); part->start--;
    part->size--;
    g_assert_false(validate_plan(disk,&error)); g_clear_error(&error); part->size++;
    part->start=101;
    g_assert_false(validate_plan(disk,&error)); g_clear_error(&error);
    disk_free(disk);
}

/* Undo copies must retain the original baseline independently of draft geometry. */
static void test_snapshot(void)
{
    Disk *disk=sample(), *copy=disk_copy(disk);
    ((Partition *)copy->parts->pdata[0])->size=500;
    g_assert_cmpuint(((Partition *)disk->parts->pdata[0])->size,==,1000);
    g_assert_false(disk_changed(disk)); g_assert_true(disk_changed(copy));
    disk_free(copy); disk_free(disk);
}

/* Whole-device filesystems are inventory entries, not newly requested partitions. */
static void test_whole_filesystem(void)
{
    Disk *disk=disk_new("/dev/example"); Partition *whole=partition_new();
    whole->size=10000; g_free(whole->fs); whole->fs=g_strdup("squashfs");
    g_ptr_array_add(disk->parts,whole);
    g_ptr_array_unref(disk->base); disk->base=parts_copy(disk->parts);
    g_assert_false(layout_changed(disk)); g_assert_false(disk_changed(disk));
    disk->new_table=TRUE; g_assert_true(disk_changed(disk));
    disk_free(disk);
}

/* Recreating the same partition number is a create/delete pair, not a forbidden move. */
static void test_recreate(void)
{
    Disk *disk=sample(); Partition *part=disk->parts->pdata[1];
    part->origin=0; part->start+=10; part->size-=20;
    GError *error=NULL; g_assert_true(validate_plan(disk,&error));
    char *plan=plan_description(disk);
    g_assert_nonnull(strstr(plan,"Delete partition 2")); g_assert_nonnull(strstr(plan,"Create partition 2"));
    g_free(plan); disk_free(disk);
}

/* Script names cannot inject headers; non-ASCII names and GPT attributes survive. */
static void test_script(void)
{
    Disk *disk=sample(); Partition *part=disk->parts->pdata[0];
    g_free(part->name); part->name=g_strdup("数据 \"D\"\nlabel: dos");
    g_free(part->attrs); part->attrs=g_strdup("RequiredPartition GUID:60");
    char *script=disk_script(disk);
    g_assert_nonnull(strstr(script,"数据 \\\"D\\\" label: dos"));
    g_assert_nonnull(strstr(script,"attrs=\"RequiredPartition GUID:60\""));
    g_assert_null(strstr(script,"\nlabel: dos"));
    g_free(script); disk_free(disk);
}

/* Device artwork follows semantic type and transport, with RAID taking priority. */
static void test_icons(void)
{
    Disk *disk=sample(); g_assert_cmpstr(device_icon(disk),==,"hard-drive");
    g_free(disk->transport); disk->transport=g_strdup("usb"); g_assert_cmpstr(device_icon(disk),==,"usb");
    g_free(disk->kind); disk->kind=g_strdup("raid1"); g_assert_cmpstr(device_icon(disk),==,"server");
    g_free(disk->kind); disk->kind=g_strdup("lvm"); g_assert_cmpstr(device_icon(disk),==,"layer-group");
    disk_free(disk);
}

/* Tests fail immediately with the real tool's diagnostic rather than continuing writes. */
static void run(const char *const *argv)
{
    GError *error=NULL;
    if (!command_run(argv,NULL,-1,NULL,NULL,&error)) g_error("%s",error->message);
}

/* A fully owned sparse file exercises actual sfdisk script parsing and readback. */
static void test_file_tables(void)
{
    ApplyOptions options={0};
    GError *error=NULL; g_assert_true(engine_init(&error));
    char *directory=g_dir_make_tmp("partitions-test-XXXXXX",&error), *path=g_build_filename(directory,"disk.img",NULL), *backup=g_build_filename(directory,"backups",NULL);
    int fd=open(path,O_CREAT|O_RDWR,0600); g_assert_cmpint(fd,>=,0); g_assert_cmpint(ftruncate(fd,128*1048576),==,0); close(fd);
    for (guint mode=0; mode<2; mode++)
    {
        Disk *disk=engine_read(path,&error); if (!disk) g_error("%s",error->message);
        g_assert_true(engine_new_table(disk,mode?"dos":"gpt",&error));
        Partition *part=partition_new(); part->number=1; part->start=disk->first; part->size=32768;
        g_free(part->type); part->type=g_strdup(filesystem_type(disk,"ext4"));
        if (!mode) { g_free(part->name); part->name=g_strdup("Data 数据"); g_free(part->attrs); part->attrs=g_strdup("RequiredPartition GUID:60"); }
        else part->bootable=TRUE;
        unsigned char boot[440]; memset(boot,0x5a,sizeof boot);
        fd=open(path,O_RDWR); g_assert_cmpint(pwrite(fd,boot,sizeof boot,0),==,sizeof boot); close(fd);
        g_ptr_array_add(disk->parts,part); GString *log=g_string_new(NULL);
        if (!engine_apply(disk,backup,log,&options,&error)) g_error("%s\n%s",error->message,log->str);
        disk_free(disk); disk=engine_read(path,&error); g_assert_nonnull(disk);
        g_assert_cmpuint(disk->parts->len,==,1); g_assert_cmpuint(((Partition *)disk->parts->pdata[0])->number,==,1);
        g_assert_cmpuint(((Partition *)disk->parts->pdata[0])->size,==,32768);
        unsigned char saved[440]; fd=open(path,O_RDONLY); g_assert_cmpint(read(fd,saved,sizeof saved),==,sizeof saved); close(fd); g_assert_cmpmem(boot,sizeof boot,saved,sizeof saved);
        g_assert_false(disk_changed(disk));
        if (!mode)
        {
            fd=open(path,O_RDWR); g_assert_cmpint(ftruncate(fd,256*1048576),==,0); close(fd); disk_free(disk);
            disk=engine_read(path,&error); g_assert_nonnull(disk); g_assert_cmpuint(disk->last,<,capacity_last(disk));
            disk->repair=TRUE; disk->last=capacity_last(disk);
            if (!engine_apply(disk,backup,log,&options,&error)) g_error("GPT repair: %s\n%s",error->message,log->str);
            disk_free(disk); disk=engine_read(path,&error); g_assert_cmpuint(disk->last,==,capacity_last(disk));
        }
        disk_free(disk); g_string_free(log,TRUE);
    }
    g_test_message("File-image test workspace: %s",directory);
    const char *remove[]={"rm","-rf","--",directory,NULL}; run(remove);
    g_free(directory); g_free(path); g_free(backup);
}

/* Replacing a whole-device filesystem must remove its signature for GPT and MBR. */
static void test_new_table_signatures(void)
{
    GError *error=NULL; g_assert_true(engine_init(&error)); ApplyOptions options={0};
    char *directory=g_dir_make_tmp("partitions-test-XXXXXX",&error), *path=g_build_filename(directory,"disk.img",NULL), *backup=g_build_filename(directory,"backups",NULL);
    for (guint mode=0;mode<2;mode++)
    {
        int fd=open(path,O_CREAT|O_TRUNC|O_RDWR,0600); g_assert_cmpint(fd,>=,0); g_assert_cmpint(ftruncate(fd,128*1048576),==,0); close(fd);
        const char *format[]={"mkfs.ext4","-F",path,NULL}; run(format);
        Disk *disk=engine_read(path,&error); g_assert_nonnull(disk);
        g_assert_true(engine_new_table(disk,mode?"dos":"gpt",&error)); GString *log=g_string_new(NULL);
        if (!engine_apply(disk,backup,log,&options,&error)) g_error("%s\n%s",error->message,log->str);
        const char *probe[]={"blkid","-p","-o","export",path,NULL}; char *output=NULL;
        g_assert_true(command_run(probe,NULL,-1,&output,NULL,&error));
        g_assert_nonnull(strstr(output,mode?"PTTYPE=dos":"PTTYPE=gpt")); g_assert_null(strstr(output,"TYPE=ext4"));
        g_free(output); disk_free(disk); g_string_free(log,TRUE);
    }
    const char *remove[]={"rm","-rf","--",directory,NULL}; run(remove);
    g_free(directory); g_free(path); g_free(backup);
}

/* Construct a new desired partition using only the current sfdisk catalogue. */
static void add_partition(Disk *disk, guint number, guint start_mib, guint size_mib, const char *fs)
{
    Partition *part=partition_new(); part->number=number; part->start=(guint64)start_mib*1048576/disk->sector; part->size=(guint64)size_mib*1048576/disk->sector;
    g_free(part->type); part->type=g_strdup(filesystem_type(disk,fs)); g_assert_nonnull(part->type);
    g_free(part->format); part->format=g_strdup(fs); g_ptr_array_add(disk->parts,part);
}

/* Integration writes are allowed only to a loop backed by this test's /tmp file. */
static int integration(const char *path)
{
    ApplyOptions options={0};
    g_assert_true(g_str_has_prefix(path,"/dev/loop"));
    char *sys=g_strdup_printf("/sys/class/block/%s/loop/backing_file",strrchr(path,'/')+1), *backing=NULL;
    g_assert_true(g_file_get_contents(sys,&backing,NULL,NULL));
    g_assert_true(g_str_has_prefix(backing,"/tmp/partitions-test-"));
    g_free(sys); g_free(backing);
    GError *error=NULL; g_assert_true(engine_init(&error));
    Disk *disk=engine_read(path,&error); if (!disk) g_error("%s",error->message);
    GPtrArray *hidden=engine_scan(path,FALSE,&error), *visible=engine_scan(path,TRUE,&error);
    g_assert_nonnull(hidden); g_assert_cmpuint(hidden->len,==,0);
    g_assert_nonnull(visible); g_assert_cmpuint(visible->len,==,1);
    g_assert_cmpstr(((Disk *)visible->pdata[0])->path,==,path);
    g_ptr_array_unref(hidden); g_ptr_array_unref(visible);
    g_assert_true(engine_new_table(disk,"gpt",&error));
    add_partition(disk,1,1,128,"ext4"); add_partition(disk,2,257,512,"vfat");
    add_partition(disk,3,800,320,"xfs"); add_partition(disk,4,1200,256,"btrfs");
    add_partition(disk,5,1664,64,"ntfs"); add_partition(disk,6,1800,128,"exfat");
    GString *log=g_string_new(NULL); char *backup=g_dir_make_tmp("partitions-test-backups-XXXXXX",&error);
    if (!engine_apply(disk,backup,log,&options,&error)) g_error("Initial format: %s\n%s",error->message,log->str);
    disk_free(disk); disk=engine_read(path,&error);
    g_assert_cmpstr(((Partition *)disk->parts->pdata[0])->fs,==,"ext4");
    g_assert_cmpstr(((Partition *)disk->parts->pdata[1])->fs,==,"vfat");
    g_assert_cmpstr(find_part(disk->parts,3)->fs,==,"xfs");
    g_assert_cmpstr(find_part(disk->parts,4)->fs,==,"btrfs");
    g_assert_cmpstr(find_part(disk->parts,5)->fs,==,"ntfs");
    g_assert_cmpstr(find_part(disk->parts,6)->fs,==,"exfat");
    char *mount=g_dir_make_tmp("partitions-test-mount-XXXXXX",&error), *node=g_strdup_printf("%sp1",path), *marker=g_build_filename(mount,"keep-this-file.txt",NULL);
    const char *mount_argv[]={"mount",node,mount,NULL}, *umount[]={"umount",mount,NULL}; run(mount_argv);
    g_assert_true(g_file_set_contents(marker,"Data survives filesystem growth.\n",-1,&error)); run(umount);
    Partition *part=disk->parts->pdata[0]; part->size=192*1048576/disk->sector; part->grow=TRUE;
    find_part(disk->parts,3)->size=384*1048576/disk->sector; find_part(disk->parts,3)->grow=TRUE;
    find_part(disk->parts,4)->size=384*1048576/disk->sector; find_part(disk->parts,4)->grow=TRUE;
    find_part(disk->parts,5)->size=96*1048576/disk->sector; find_part(disk->parts,5)->grow=TRUE;
    if (!engine_apply(disk,backup,log,&options,&error)) g_error("Grow ext4: %s\n%s",error->message,log->str);
    disk_free(disk); disk=engine_read(path,&error); part=disk->parts->pdata[0];
    g_assert_cmpuint(part->fsbytes,==,192*1048576);
    g_assert_cmpuint(find_part(disk->parts,3)->fsbytes,==,384*1048576);
    g_assert_cmpuint(find_part(disk->parts,4)->fsbytes,==,384*1048576);
    g_assert_cmpuint(find_part(disk->parts,5)->fsbytes,>=,95*1048576);
    run(mount_argv); char *contents=NULL; g_assert_true(g_file_get_contents(marker,&contents,NULL,&error));
    g_assert_cmpstr(contents,==,"Data survives filesystem growth.\n"); g_free(contents);
    part->size=224*1048576/disk->sector;
    g_assert_false(engine_apply(disk,backup,log,&options,&error)); g_assert_nonnull(strstr(error->message,"in use")); g_clear_error(&error); run(umount);
    int lock=open(path,O_RDWR); g_assert_cmpint(flock(lock,LOCK_EX),==,0);
    g_assert_false(engine_apply(disk,backup,log,&options,&error)); g_assert_nonnull(strstr(error->message,"Another program")); g_clear_error(&error); close(lock);
    const char *rename[]={"sfdisk","--part-label",path,"1","Externally changed",NULL}; run(rename);
    g_assert_false(engine_apply(disk,backup,log,&options,&error)); g_assert_nonnull(strstr(error->message,"changed")); g_clear_error(&error);
    disk_free(disk); disk=engine_read(path,&error);
    g_assert_cmpuint(((Partition *)disk->parts->pdata[0])->size,==,192*1048576/disk->sector);
    run(mount_argv); options.allow_busy=TRUE;
    part=find_part(disk->parts,1); part->size=224*1048576/disk->sector; part->grow=TRUE;
    if (!engine_apply(disk,backup,log,&options,&error)) g_error("Mounted ext4 growth: %s\n%s",error->message,log->str);
    disk_free(disk); disk=engine_read(path,&error); g_assert_cmpuint(find_part(disk->parts,1)->fsbytes,==,224*1048576);
    for (guint number=3;number<=4;number++)
    {
        char *fsnode=g_strdup_printf("%sp%u",path,number), *fsmount=g_dir_make_tmp("partitions-test-mount-XXXXXX",&error);
        const char *attach[]={"mount",fsnode,fsmount,NULL}, *detach[]={"umount",fsmount,NULL}; run(attach);
        disk_free(disk); disk=engine_read(path,&error); Partition *grown=find_part(disk->parts,number);
        grown->size=(guint64)(number==3?392:416)*1048576/disk->sector; grown->grow=TRUE;
        if (!engine_apply(disk,backup,log,&options,&error)) g_error("Mounted filesystem growth: %s\n%s",error->message,log->str);
        run(detach); rmdir(fsmount); g_free(fsnode); g_free(fsmount);
        disk_free(disk); disk=engine_read(path,&error);
        g_assert_cmpuint(find_part(disk->parts,number)->fsbytes,==,(guint64)(number==3?392:416)*1048576);
    }
    add_partition(disk,7,225,16,"ext4");
    if (!engine_apply(disk,backup,log,&options,&error)) g_error("Create beside mounted source: %s\n%s",error->message,log->str);
    g_assert_true(g_file_get_contents(marker,&contents,NULL,&error)); g_assert_cmpstr(contents,==,"Data survives filesystem growth.\n"); g_free(contents);
    disk_free(disk); disk=engine_read(path,&error);
    g_assert_cmpstr(find_part(disk->parts,7)->fs,==,"ext4");
    for (guint i=0;i<disk->parts->len;i++) if (((Partition *)disk->parts->pdata[i])->number==1) { g_ptr_array_remove_index(disk->parts,i); break; }
    if (!engine_apply(disk,backup,log,&options,&error)) g_error("Delete mounted entry: %s\n%s",error->message,log->str);
    g_assert_true(options.reboot_required);
    g_assert_true(g_file_get_contents(marker,&contents,NULL,&error)); g_free(contents); run(umount);
    g_print("PASS: GPT, all six formatters, ext4/XFS/Btrfs/NTFS growth, preserved data, mounted ext4 online growth, creating beside a mounted source, deleting a mounted entry with restart required, lock and stale-plan rejection.\n%s",log->str);
    disk_free(disk); g_string_free(log,TRUE); rmdir(mount);
    const char *remove[]={"rm","-rf","--",backup,NULL}; run(remove);
    g_free(backup); g_free(mount); g_free(node); g_free(marker); return 0;
}

/* Run model/file tests by default; loop integration requires its explicit argument. */
int main(int argc, char **argv)
{
    if (argc==3 && !strcmp(argv[1],"--integration")) return integration(argv[2]);
    g_test_init(&argc,&argv,NULL);
    g_test_add_func("/model/free-consolidation",test_merge);
    g_test_add_func("/model/geometry-validation",test_validation);
    g_test_add_func("/model/independent-snapshots",test_snapshot);
    g_test_add_func("/model/whole-device-filesystem",test_whole_filesystem);
    g_test_add_func("/model/delete-and-recreate",test_recreate);
    g_test_add_func("/model/quoted-script",test_script);
    g_test_add_func("/model/device-icons",test_icons);
    g_test_add_func("/engine/file-tables-and-gpt-repair",test_file_tables);
    g_test_add_func("/engine/new-table-removes-filesystem-signature",test_new_table_signatures);
    return g_test_run();
}
