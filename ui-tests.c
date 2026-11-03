#include "partition-manager.h"
#include <gtk/gtk.h>
#include <string.h>

static guint phase, attempts;
static GtkWidget *workspace;

/* Inspect only this process's native widget tree, never the user's desktop. */
static GtkWidget *find(GtkWidget *root, GType type, const char *action)
{
    if (g_type_is_a(G_OBJECT_TYPE(root),type))
    {
        const char *command=g_object_get_data(G_OBJECT(root),"action");
        if (!action || (command && !strcmp(command,action))) return root;
    }
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children=gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result=NULL;
    for (GList *item=children; item && !result; item=item->next) result=find(item->data,type,action);
    g_list_free(children); return result;
}

/* Find the partition list by its model-backed rows, independent of coordinates. */
static GtkWidget *partition_list(GtkWidget *root)
{
    if (GTK_IS_LIST_BOX(root))
    {
        GtkListBoxRow *row=gtk_list_box_get_row_at_index(GTK_LIST_BOX(root),0);
        if (row && g_object_get_data(G_OBJECT(row),"item")) return root;
    }
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children=gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result=NULL;
    for (GList *item=children; item && !result; item=item->next) result=partition_list(item->data);
    g_list_free(children); return result;
}

/* Capacity labels are semantic CSS elements rather than brittle pixel locations. */
static GtkWidget *capacity(GtkWidget *root)
{
    if (GTK_IS_LABEL(root) && gtk_style_context_has_class(gtk_widget_get_style_context(root),"capacity")) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children=gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result=NULL;
    for (GList *item=children; item && !result; item=item->next) result=capacity(item->data);
    g_list_free(children); return result;
}

/* Modal editors are separate native toplevels in GTK3. */
static GtkWidget *dialog(void)
{
    GList *windows=gtk_window_list_toplevels(); GtkWidget *result=NULL;
    for (GList *item=windows; item; item=item->next)
        if (GTK_IS_DIALOG(item->data) && gtk_widget_get_visible(item->data)) result=item->data;
    g_list_free(windows); return result;
}

/* Save real GTK rendering to a temporary QA artifact. */
static void capture(GtkWidget *widget, const char *path)
{
    GtkRequisition minimum,natural; GtkAllocation allocation;
    gtk_widget_get_preferred_size(widget,&minimum,&natural); gtk_widget_get_allocation(widget,&allocation); gtk_widget_size_allocate(widget,&allocation);
    GtkWidget *header=gtk_window_get_titlebar(GTK_WINDOW(widget)), *body=gtk_bin_get_child(GTK_BIN(widget)); int x=0,y=0;
    gtk_widget_translate_coordinates(header,widget,0,0,&x,&y);
    int width=gtk_widget_get_allocated_width(header)+2, height=gtk_widget_get_allocated_height(header)+gtk_widget_get_allocated_height(body)+2;
    cairo_surface_t *surface=cairo_image_surface_create(CAIRO_FORMAT_ARGB32,width,height); cairo_t *cr=cairo_create(surface);
    cairo_translate(cr,1-x,1-y); gtk_widget_draw(widget,cr); cairo_destroy(cr);
    GdkPixbuf *pixbuf=gdk_pixbuf_get_from_surface(surface,0,0,width,height); cairo_surface_destroy(surface);
    GError *error=NULL; g_assert_nonnull(pixbuf); g_assert_true(gdk_pixbuf_save(pixbuf,path,"png",&error,NULL)); g_object_unref(pixbuf);
}

/* Exercise the same pointer callbacks used by real mouse events. */
static void drag(GtkWidget *area, double from, double to)
{
    gboolean handled=FALSE;
    GdkEventButton press={.type=GDK_BUTTON_PRESS,.button=1,.x=from,.y=45};
    GdkEventMotion move={.type=GDK_MOTION_NOTIFY,.state=GDK_BUTTON1_MASK,.x=to,.y=45};
    GdkEventButton release={.type=GDK_BUTTON_RELEASE,.button=1,.x=to,.y=45};
    g_signal_emit_by_name(area,"button-press-event",&press,&handled); g_assert_true(handled);
    g_signal_emit_by_name(area,"motion-notify-event",&move,&handled);
    g_signal_emit_by_name(area,"button-release-event",&release,&handled);
}

/* A reentrant timer continues through gtk_dialog_run's nested main loops. */
static gboolean step(gpointer data)
{
    (void)data;
    g_timeout_add(150,step,NULL);
    if (++attempts>150) g_error("GTK smoke test timed out in phase %u",phase);
    if (!workspace)
    {
        GList *windows=gtk_window_list_toplevels();
        for (GList *item=windows; item; item=item->next) if (!GTK_IS_DIALOG(item->data) && gtk_widget_get_visible(item->data)) workspace=item->data;
        g_list_free(windows);
        if (!workspace) return FALSE;
    }
    GtkWidget *list=partition_list(workspace), *modal=dialog();
    if (phase==0)
    {
        if (!list) return FALSE;
        g_assert_null(modal); capture(workspace,"/tmp/partitions-native-workspace.png");
        gtk_list_box_select_row(GTK_LIST_BOX(list),gtk_list_box_get_row_at_index(GTK_LIST_BOX(list),1));
        GtkWidget *create=find(workspace,GTK_TYPE_BUTTON,"create"); g_assert_true(gtk_widget_is_sensitive(create));
        phase=1; gtk_button_clicked(GTK_BUTTON(create));
    }
    else if (phase==1)
    {
        if (!modal) return FALSE;
        g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(modal)),==,"New partition");
        GtkWidget *area=find(modal,GTK_TYPE_DRAWING_AREA,NULL), *size=capacity(modal);
        char *initial=g_strdup(gtk_label_get_text(GTK_LABEL(size)));
        double width=gtk_widget_get_allocated_width(area);
        drag(area,width-12,width-180); drag(area,100,140); drag(area,52,84);
        g_assert_cmpstr(initial,!=,gtk_label_get_text(GTK_LABEL(size))); g_free(initial);
        capture(modal,"/tmp/partitions-native-create.png");
        phase=2; gtk_dialog_response(GTK_DIALOG(modal),GTK_RESPONSE_ACCEPT);
    }
    else if (phase==2)
    {
        if (modal) return FALSE;
        GtkWidget *edit=find(workspace,GTK_TYPE_BUTTON,"edit");
        GtkWidget *caption=g_object_get_data(G_OBJECT(edit),"caption"); g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(caption)),==,"Adjust");
        capture(workspace,"/tmp/partitions-native-plan.png"); phase=20; gtk_button_clicked(GTK_BUTTON(find(workspace,GTK_TYPE_BUTTON,"review")));
    }
    else if (phase==20)
    {
        if (!modal) return FALSE;
        g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(modal)),==,"Review changes");
        GtkWidget *apply=gtk_dialog_get_widget_for_response(GTK_DIALOG(modal),GTK_RESPONSE_ACCEPT), *checkbox=find(modal,GTK_TYPE_CHECK_BUTTON,NULL);
        g_assert_nonnull(checkbox); g_assert_false(gtk_widget_is_sensitive(apply));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkbox),TRUE); g_assert_true(gtk_widget_is_sensitive(apply));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkbox),FALSE); g_assert_false(gtk_widget_is_sensitive(apply));
        capture(modal,"/tmp/partitions-native-review.png"); phase=21; gtk_dialog_response(GTK_DIALOG(modal),GTK_RESPONSE_CANCEL);
    }
    else if (phase==21)
    {
        if (modal) return FALSE;
        phase=3; gtk_button_clicked(GTK_BUTTON(find(workspace,GTK_TYPE_BUTTON,"edit")));
    }
    else if (phase==3)
    {
        if (!modal) return FALSE;
        g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(modal)),==,"Adjust new partition");
        capture(modal,"/tmp/partitions-native-adjust.png");
        phase=4; gtk_dialog_response(GTK_DIALOG(modal),GTK_RESPONSE_CANCEL);
    }
    else if (phase==4)
    {
        if (modal) return FALSE;
        gtk_button_clicked(GTK_BUTTON(find(workspace,GTK_TYPE_BUTTON,"undo")));
        list=partition_list(workspace);
        gtk_list_box_select_row(GTK_LIST_BOX(list),gtk_list_box_get_row_at_index(GTK_LIST_BOX(list),0));
        GtkWidget *grow=find(workspace,GTK_TYPE_BUTTON,"grow"); g_assert_true(gtk_widget_is_sensitive(grow));
        phase=5; gtk_button_clicked(GTK_BUTTON(grow));
    }
    else if (phase==5)
    {
        if (!modal) return FALSE;
        g_assert_true(g_str_has_prefix(gtk_window_get_title(GTK_WINDOW(modal)),"Grow "));
        GtkWidget *size=capacity(modal);
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(size)),==,"128.0 MiB");
        capture(modal,"/tmp/partitions-native-grow.png");
        phase=6; gtk_dialog_response(GTK_DIALOG(modal),GTK_RESPONSE_CANCEL);
    }
    else if (phase==6)
    {
        if (modal) return FALSE;
        g_assert_false(gtk_widget_is_sensitive(find(workspace,GTK_TYPE_BUTTON,"review")));
        phase=7; gtk_button_clicked(GTK_BUTTON(find(workspace,GTK_TYPE_BUTTON,"format")));
    }
    else if (phase==7)
    {
        if (!modal) return FALSE;
        g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(modal)),==,"Format partition"); capture(modal,"/tmp/partitions-native-format.png");
        phase=8; gtk_dialog_response(GTK_DIALOG(modal),GTK_RESPONSE_CANCEL);
    }
    else if (phase==8)
    {
        if (modal) return FALSE;
        phase=9; gtk_button_clicked(GTK_BUTTON(find(workspace,GTK_TYPE_BUTTON,"edit")));
    }
    else if (phase==9)
    {
        if (!modal) return FALSE;
        g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(modal)),==,"Rename partition"); capture(modal,"/tmp/partitions-native-rename.png");
        phase=10; gtk_dialog_response(GTK_DIALOG(modal),GTK_RESPONSE_CANCEL);
    }
    else if (phase==10)
    {
        if (modal) return FALSE;
        phase=11; gtk_button_clicked(GTK_BUTTON(find(workspace,GTK_TYPE_BUTTON,"new-table")));
    }
    else if (phase==11)
    {
        if (!modal) return FALSE;
        g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(modal)),==,"New partition table"); capture(modal,"/tmp/partitions-native-new-table.png");
        phase=12; gtk_dialog_response(GTK_DIALOG(modal),GTK_RESPONSE_CANCEL);
    }
    else if (phase==12)
    {
        if (modal) return FALSE;
        phase=13; gtk_button_clicked(GTK_BUTTON(find(workspace,GTK_TYPE_BUTTON,"info")));
    }
    else if (phase==13)
    {
        if (!modal) return FALSE;
        g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(modal)),==,"Disk properties"); capture(modal,"/tmp/partitions-native-properties.png");
        phase=14; gtk_dialog_response(GTK_DIALOG(modal),GTK_RESPONSE_ACCEPT);
    }
    else if (phase==14)
    {
        if (modal) return FALSE;
        phase=15; gtk_button_clicked(GTK_BUTTON(find(workspace,GTK_TYPE_BUTTON,"check")));
    }
    else if (phase==15)
    {
        if (!modal) return FALSE;
        g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(modal)),==,"Partition table checked"); capture(modal,"/tmp/partitions-native-check.png");
        phase=16; gtk_dialog_response(GTK_DIALOG(modal),GTK_RESPONSE_ACCEPT);
    }
    else if (phase==16)
    {
        if (modal) return FALSE;
        gtk_main_quit(); return FALSE;
    }
    return FALSE;
}

/* The test owns a loop-backed fixture and verifies that GUI planning wrote nothing. */
int main(int argc, char **argv)
{
    g_assert_cmpint(argc,==,2); g_assert_true(g_str_has_prefix(argv[1],"/dev/loop"));
    GError *error=NULL; g_assert_true(engine_init(&error));
    Disk *before=engine_read(argv[1],&error); g_assert_nonnull(before);
    g_timeout_add(150,step,NULL);
    char *args[]={argv[0],NULL}; ui_run(1,args,argv[1],FALSE);
    Disk *after=engine_read(argv[1],&error); g_assert_nonnull(after);
    g_assert_cmpstr(before->snapshot,==,after->snapshot);
    disk_free(before); disk_free(after);
    g_print("PASS: native GTK selection, two-edge resize, movement, Review with explicit in-use acknowledgement, Adjust, Undo, current-size Grow, and no disk writes.\n");
    return 0;
}
