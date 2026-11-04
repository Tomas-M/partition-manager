#include "partition-manager.h"
#include <gtk/gtk.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

static guint phase, attempts;
static GtkWidget *workspace;
static int sidebar_width, details_width, map_width, window_width, window_height, held_mount;
static GtkListBoxRow *stable_rows[2];
static guint clicks;
static char *fixture_path, *fixture_mount;

/* Find semantic layout elements without depending on window coordinates. */
static GtkWidget *styled(GtkWidget *root, const char *name)
{
    if (gtk_style_context_has_class(gtk_widget_get_style_context(root),name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children=gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result=NULL;
    for (GList *item=children; item && !result; item=item->next) result=styled(item->data,name);
    g_list_free(children); return result;
}

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

/* Icon-only controls keep their content centered at every window size. */
static void assert_icon_alignment(void)
{
    const char *actions[]={"info","save","delete"};
    for (guint i=0;i<G_N_ELEMENTS(actions);i++)
    {
        GtkWidget *button=find(workspace,GTK_TYPE_BUTTON,actions[i]), *content=gtk_bin_get_child(GTK_BIN(button));
        int x,y; gtk_widget_translate_coordinates(content,button,0,0,&x,&y);
        g_assert_cmpint(abs(2*x+gtk_widget_get_allocated_width(content)-gtk_widget_get_allocated_width(button)),<=,1);
        g_assert_cmpint(abs(2*y+gtk_widget_get_allocated_height(content)-gtk_widget_get_allocated_height(button)),<=,1);
    }
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

/* Closing Review must leave its in-flight usage check safe to finish. */
static gboolean cancel_review(gpointer data)
{
    (void)data; GtkWidget *modal=dialog(); g_assert_nonnull(modal);
    g_assert_false(gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(modal),GTK_RESPONSE_ACCEPT)));
    gtk_dialog_response(GTK_DIALOG(modal),GTK_RESPONSE_CANCEL); return G_SOURCE_REMOVE;
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

/* Queue complete pointer events through GTK's list-box gesture and release handling. */
static void click_row(GtkListBoxRow *row)
{
    GdkWindow *window=gtk_widget_get_window(GTK_WIDGET(row)); GtkWidget *owner=NULL;
    gdk_window_get_user_data(window,(gpointer *)&owner);
    int x,y; gtk_widget_translate_coordinates(GTK_WIDGET(row),owner,20,gtk_widget_get_allocated_height(GTK_WIDGET(row))/2,&x,&y);
    for (guint i=0;i<2;i++)
    {
        GdkEvent *event=gdk_event_new(i?GDK_BUTTON_RELEASE:GDK_BUTTON_PRESS);
        event->button.window=g_object_ref(window); event->button.button=1; event->button.x=x; event->button.y=y;
        event->button.state=i?GDK_BUTTON1_MASK:0;
        gdk_event_set_device(event,gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_window_get_display(window))));
        gdk_event_put(event); gdk_event_free(event);
    }
}

/* Dispatch a double-click at a known fixture interval in the graphical map. */
static void double_click_map(GtkWidget *map, double x)
{
    gboolean handled=FALSE;
    GdkEventButton event={.type=GDK_2BUTTON_PRESS,.button=1,.x=x,.y=30};
    g_signal_emit_by_name(map,"button-press-event",&event,&handled); g_assert_true(handled);
}

/* A reentrant timer continues through gtk_dialog_run's nested main loops. */
static gboolean step(gpointer data)
{
    g_timeout_add(150,step,data);
    if (++attempts>150) g_error("GTK smoke test timed out in phase %u",phase);
    if (!workspace)
    {
        GList *windows=gtk_window_list_toplevels();
        for (GList *item=windows; item; item=item->next) if (!GTK_IS_DIALOG(item->data) && gtk_widget_get_visible(item->data)) workspace=item->data;
        g_list_free(windows);
        if (!workspace) return FALSE;
    }
    GtkWidget *list=partition_list(workspace), *modal=dialog();
    if (data)
    {
        if (!list) return FALSE;
        if (phase==0)
        {
            GtkWidget *map=styled(workspace,"disk-map"); gboolean handled=FALSE;
            GdkEventButton event={.type=GDK_2BUTTON_PRESS,.button=1,.x=gtk_widget_get_allocated_width(map)/2,.y=30};
            phase=1; g_signal_emit_by_name(map,"button-press-event",&event,&handled); g_assert_true(handled);
        }
        else if (phase==1 || phase==3)
        {
            if (!modal) return FALSE;
            g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(modal)),==,"New partition table");
            int response=phase==3?GTK_RESPONSE_ACCEPT:GTK_RESPONSE_CANCEL;
            phase++; gtk_dialog_response(GTK_DIALOG(modal),response);
        }
        else if (phase==2)
        {
            if (modal) return FALSE;
            GtkListBoxRow *row=gtk_list_box_get_row_at_index(GTK_LIST_BOX(list),0);
            phase=3; click_row(row); click_row(row);
        }
        else if (phase==4 && !modal)
        {
            g_assert_true(gtk_widget_is_sensitive(find(workspace,GTK_TYPE_BUTTON,"review")));
            gtk_button_clicked(GTK_BUTTON(find(workspace,GTK_TYPE_BUTTON,"undo")));
            g_assert_false(gtk_widget_is_sensitive(find(workspace,GTK_TYPE_BUTTON,"review")));
            gtk_main_quit();
        }
        return FALSE;
    }
    if (phase==0)
    {
        if (!list) return FALSE;
        assert_icon_alignment();
        g_assert_null(modal); capture(workspace,"/tmp/partitions-native-workspace.png");
        gtk_list_box_select_row(GTK_LIST_BOX(list),gtk_list_box_get_row_at_index(GTK_LIST_BOX(list),1));
        GtkWidget *create=find(workspace,GTK_TYPE_BUTTON,"create"); g_assert_true(gtk_widget_is_sensitive(create));
        phase=1; GtkListBoxRow *row=gtk_list_box_get_selected_row(GTK_LIST_BOX(list)); click_row(row); click_row(row);
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
        GtkWidget *resize=find(workspace,GTK_TYPE_BUTTON,"resize");
        GtkWidget *caption=g_object_get_data(G_OBJECT(resize),"caption"); g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(caption)),==,"Resize");
        capture(workspace,"/tmp/partitions-native-plan.png"); phase=20; gtk_button_clicked(GTK_BUTTON(find(workspace,GTK_TYPE_BUTTON,"review")));
    }
    else if (phase==20)
    {
        if (!modal || gtk_widget_get_visible(find(modal,GTK_TYPE_SPINNER,NULL))) return FALSE;
        g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(modal)),==,"Review changes");
        GtkWidget *apply=gtk_dialog_get_widget_for_response(GTK_DIALOG(modal),GTK_RESPONSE_ACCEPT), *checkbox=find(modal,GTK_TYPE_CHECK_BUTTON,NULL);
        g_assert_true(gtk_widget_get_visible(checkbox)); g_assert_false(gtk_widget_is_sensitive(apply));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkbox),TRUE); g_assert_true(gtk_widget_is_sensitive(apply));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(checkbox),FALSE); g_assert_false(gtk_widget_is_sensitive(apply));
        held_mount=open(fixture_mount,O_RDONLY); g_assert_cmpint(held_mount,>=,0);
        capture(modal,"/tmp/partitions-native-review.png"); phase=69;
        gtk_button_clicked(GTK_BUTTON(find(modal,GTK_TYPE_BUTTON,"unmount")));
    }
    else if (phase==69)
    {
        if (gtk_widget_get_visible(find(modal,GTK_TYPE_SPINNER,NULL))) return FALSE;
        g_assert_nonnull(styled(modal,"in-use-warning")); g_assert_true(gtk_widget_get_visible(find(modal,GTK_TYPE_CHECK_BUTTON,NULL)));
        g_assert_false(gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(modal),GTK_RESPONSE_ACCEPT)));
        capture(modal,"/tmp/partition-manager-unmount-busy.png"); close(held_mount); phase=70;
        gtk_button_clicked(GTK_BUTTON(find(modal,GTK_TYPE_BUTTON,"unmount")));
    }
    else if (phase==70)
    {
        if (gtk_widget_get_visible(find(modal,GTK_TYPE_SPINNER,NULL))) return FALSE;
        g_assert_nonnull(styled(modal,"disk-ready"));
        g_assert_false(gtk_widget_get_visible(find(modal,GTK_TYPE_CHECK_BUTTON,NULL)));
        g_assert_true(gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(modal),GTK_RESPONSE_ACCEPT)));
        GError *error=NULL; Disk *live=engine_read(fixture_path,&error); g_assert_nonnull(live); g_assert_false(live->busy); disk_free(live);
        capture(modal,"/tmp/partition-manager-disk-ready.png"); phase=71; gtk_dialog_response(GTK_DIALOG(modal),GTK_RESPONSE_CANCEL);
    }
    else if (phase==71)
    {
        if (modal) return FALSE;
        char *node=g_strdup_printf("%sp1",fixture_path); const char *argv[]={"mount",node,fixture_mount,NULL}; GError *error=NULL;
        g_assert_true(command_run(argv,NULL,-1,NULL,NULL,&error)); g_free(node);
        phase=72; gtk_button_clicked(GTK_BUTTON(find(workspace,GTK_TYPE_BUTTON,"review")));
    }
    else if (phase==72)
    {
        if (!modal || gtk_widget_get_visible(find(modal,GTK_TYPE_SPINNER,NULL))) return FALSE;
        g_assert_nonnull(styled(modal,"in-use-warning")); g_assert_true(gtk_widget_get_visible(find(modal,GTK_TYPE_CHECK_BUTTON,NULL)));
        g_assert_false(gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(modal),GTK_RESPONSE_ACCEPT)));
        phase=74; gtk_dialog_response(GTK_DIALOG(modal),GTK_RESPONSE_CANCEL);
    }
    else if (phase==74)
    {
        if (modal) return FALSE;
        phase=75; g_idle_add(cancel_review,NULL); gtk_button_clicked(GTK_BUTTON(find(workspace,GTK_TYPE_BUTTON,"review")));
    }
    else if (phase==75)
    {
        if (modal || !gtk_widget_is_sensitive(find(workspace,GTK_TYPE_BUTTON,"review"))) return FALSE;
        phase=21;
    }
    else if (phase==21)
    {
        if (modal) return FALSE;
        phase=3; GtkListBoxRow *row=gtk_list_box_get_selected_row(GTK_LIST_BOX(list)); click_row(row); click_row(row);
    }
    else if (phase==3)
    {
        if (!modal) return FALSE;
        g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(modal)),==,"Resize new partition");
        capture(modal,"/tmp/partitions-native-adjust.png");
        phase=4; gtk_dialog_response(GTK_DIALOG(modal),GTK_RESPONSE_CANCEL);
    }
    else if (phase==4)
    {
        if (modal) return FALSE;
        gtk_button_clicked(GTK_BUTTON(find(workspace,GTK_TYPE_BUTTON,"undo")));
        list=partition_list(workspace);
        gtk_list_box_select_row(GTK_LIST_BOX(list),gtk_list_box_get_row_at_index(GTK_LIST_BOX(list),0));
        GtkWidget *resize=find(workspace,GTK_TYPE_BUTTON,"resize"); g_assert_true(gtk_widget_is_sensitive(resize));
        phase=5; double_click_map(styled(workspace,"disk-map"),12);
    }
    else if (phase==5)
    {
        if (!modal) return FALSE;
        g_assert_true(g_str_has_prefix(gtk_window_get_title(GTK_WINDOW(modal)),"Resize "));
        GtkWidget *size=capacity(modal);
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(size)),==,"128.0 MiB");
        capture(modal,"/tmp/partitions-native-grow.png");
        phase=6; gtk_dialog_response(GTK_DIALOG(modal),GTK_RESPONSE_CANCEL);
    }
    else if (phase==6)
    {
        if (modal) return FALSE;
        g_assert_false(gtk_widget_is_sensitive(find(workspace,GTK_TYPE_BUTTON,"review")));
        phase=50; GtkListBoxRow *row=gtk_list_box_get_selected_row(GTK_LIST_BOX(list)); click_row(row); click_row(row);
    }
    else if (phase==50)
    {
        if (!modal) return FALSE;
        g_assert_true(g_str_has_prefix(gtk_window_get_title(GTK_WINDOW(modal)),"Resize "));
        phase=51; gtk_dialog_response(GTK_DIALOG(modal),GTK_RESPONSE_CANCEL);
    }
    else if (phase==51)
    {
        if (modal) return FALSE;
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
        stable_rows[0]=gtk_list_box_get_row_at_index(GTK_LIST_BOX(list),0); stable_rows[1]=gtk_list_box_get_row_at_index(GTK_LIST_BOX(list),1);
        phase=30; clicks=1; click_row(stable_rows[1]);
    }
    else if (phase==30)
    {
        g_assert_true(gtk_list_box_get_selected_row(GTK_LIST_BOX(list))==stable_rows[clicks%2]);
        g_assert_true(gtk_list_box_get_row_at_index(GTK_LIST_BOX(list),0)==stable_rows[0]);
        g_assert_true(gtk_list_box_get_row_at_index(GTK_LIST_BOX(list),1)==stable_rows[1]);
        if (++clicks<=20) click_row(stable_rows[clicks%2]);
        else phase=31;
    }
    else if (phase==31)
    {
        gtk_window_get_size(GTK_WINDOW(workspace),&window_width,&window_height);
        sidebar_width=gtk_widget_get_allocated_width(styled(workspace,"sidebar"));
        details_width=gtk_widget_get_allocated_width(styled(workspace,"details"));
        map_width=gtk_widget_get_allocated_width(styled(workspace,"disk-map"));
        gtk_window_resize(GTK_WINDOW(workspace),900,480); phase=17;
    }
    else if (phase==17 || phase==18)
    {
        int width,height; gtk_window_get_size(GTK_WINDOW(workspace),&width,&height);
        int requested_width=phase==17?900:820, requested_height=phase==17?480:360;
        if (width!=requested_width || height!=requested_height) return FALSE;
        g_assert_cmpint(gtk_widget_get_allocated_width(styled(workspace,"sidebar")),==,sidebar_width);
        g_assert_cmpint(gtk_widget_get_allocated_width(styled(workspace,"details")),==,details_width);
        g_assert_cmpint(gtk_widget_get_allocated_width(styled(workspace,"disk-map")),<,map_width);
        GtkWidget *scroll=gtk_widget_get_ancestor(list,GTK_TYPE_SCROLLED_WINDOW);
        GtkAdjustment *vertical=gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
        g_assert_cmpfloat(gtk_adjustment_get_upper(vertical),>,gtk_adjustment_get_page_size(vertical));
        GtkWidget *columns=find(styled(workspace,"table-header"),GTK_TYPE_GRID,NULL);
        GtkWidget *name=gtk_grid_get_child_at(GTK_GRID(columns),0,0), *filesystem=gtk_grid_get_child_at(GTK_GRID(columns),2,0);
        g_assert_cmpint(abs(gtk_widget_get_allocated_width(filesystem)*2-gtk_widget_get_allocated_width(name)-gtk_widget_get_margin_end(name)),<=,2);
        GtkWidget *details_scroll=gtk_widget_get_ancestor(styled(workspace,"details"),GTK_TYPE_SCROLLED_WINDOW);
        g_assert_true(gtk_scrolled_window_get_overlay_scrolling(GTK_SCROLLED_WINDOW(details_scroll)));
        GtkAdjustment *properties=gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(details_scroll));
        g_assert_cmpfloat(gtk_adjustment_get_upper(properties),>,gtk_adjustment_get_page_size(properties));
        gtk_adjustment_set_value(properties,gtk_adjustment_get_upper(properties)-gtk_adjustment_get_page_size(properties));
        g_assert_cmpfloat(gtk_adjustment_get_value(properties),>,0);
        capture(workspace,phase==17?"/tmp/partition-manager-900x480.png":"/tmp/partition-manager-820x360.png");
        if (phase==17) { gtk_window_resize(GTK_WINDOW(workspace),820,360); phase=18; }
        else { gtk_window_resize(GTK_WINDOW(workspace),window_width,window_height); phase=19; }
    }
    else if (phase==19)
    {
        if (gtk_widget_get_allocated_width(styled(workspace,"disk-map"))!=map_width) return FALSE;
        assert_icon_alignment(); phase=60;
        GtkWidget *map=styled(workspace,"disk-map"); double_click_map(map,gtk_widget_get_allocated_width(map)-20);
    }
    else if (phase==60)
    {
        if (!modal) return FALSE;
        g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(modal)),==,"New partition");
        phase=61; gtk_dialog_response(GTK_DIALOG(modal),GTK_RESPONSE_ACCEPT);
    }
    else if (phase==61)
    {
        if (modal) return FALSE;
        GtkWidget *map=styled(workspace,"disk-map"); phase=62; double_click_map(map,gtk_widget_get_allocated_width(map)-20);
    }
    else if (phase==62)
    {
        if (!modal) return FALSE;
        g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(modal)),==,"Resize new partition");
        phase=63; gtk_dialog_response(GTK_DIALOG(modal),GTK_RESPONSE_CANCEL);
    }
    else if (phase==63)
    {
        if (modal) return FALSE;
        gtk_button_clicked(GTK_BUTTON(find(workspace,GTK_TYPE_BUTTON,"undo")));
        g_assert_false(gtk_widget_is_sensitive(find(workspace,GTK_TYPE_BUTTON,"review")));
        gtk_main_quit(); return FALSE;
    }
    return FALSE;
}

/* The test owns a loop-backed fixture and verifies that GUI planning left the partition table unchanged. */
int main(int argc, char **argv)
{
    gboolean empty_table=argc==3 && !strcmp(argv[1],"--empty-table");
    g_assert_cmpint(argc,==,empty_table?3:2); const char *path=argv[argc-1];
    g_assert_true(g_str_has_prefix(path,empty_table?"/tmp/":"/dev/loop"));
    GError *error=NULL; g_assert_true(engine_init(&error));
    Disk *before=engine_read(path,&error); g_assert_nonnull(before);
    fixture_path=g_strdup(path);
    if (!empty_table) fixture_mount=g_strdup(((Partition *)before->parts->pdata[0])->mount);
    g_timeout_add(150,step,GINT_TO_POINTER(empty_table));
    char *args[]={argv[0],NULL}; ui_run(1,args,path,FALSE,TRUE);
    Disk *after=engine_read(path,&error); g_assert_nonnull(after);
    g_assert_cmpstr(before->snapshot,==,after->snapshot);
    disk_free(before); disk_free(after); g_free(fixture_path); g_free(fixture_mount);
    g_print("PASS: native GTK selection and mouse releases, two-edge resize, movement, Review with explicit in-use acknowledgement, Resize, Undo, current-size growth, responsive window layout with fixed side panes, map and row double-click actions, safe unmount and fresh usage checks, and no partition-table writes.\n");
    return 0;
}
