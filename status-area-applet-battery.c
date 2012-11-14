 /***********************************************************************************
 *  status-area-applet-battery: Open source rewrite of the Maemo 5 battery applet
 *  Copyright (C) 2011 Mohammad Abu-Garbeyyeh
 *  Copyright (C) 2011-2012 Pali Rohár
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 ***********************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <libintl.h>

#include <dbus/dbus.h>
#include <hal/libhal.h>
#include <profiled/libprofile.h>
#include <canberra.h>

#include <gtk/gtk.h>
#include <hildon/hildon.h>
#include <libhildondesktop/libhildondesktop.h>

#define HAL_BQ_UDI "/org/freedesktop/Hal/devices/computer_power_supply_battery_bq27200_0"
#define HAL_RX_UDI "/org/freedesktop/Hal/devices/computer_power_supply_battery_rx51_battery"
#define HAL_BME_UDI "/org/freedesktop/Hal/devices/bme"
#define HAL_PERCENTAGE_KEY "battery.charge_level.percentage"
#define HAL_CAPACITY_KEY "battery.charge_level.capacity_state"
#define HAL_CURRENT_KEY "battery.reporting.current"
#define HAL_DESIGN_KEY "battery.reporting.design"
#define HAL_TIME_KEY "battery.remaining_time"
#define HAL_BARS_KEY "battery.charge_level.current"
#define HAL_IS_CHARGING_KEY "battery.rechargeable.is_charging"
#define HAL_IS_DISCHARGING_KEY "battery.rechargeable.is_discharging"

G_BEGIN_DECLS

typedef struct _BatteryStatusAreaItem        BatteryStatusAreaItem;
typedef struct _BatteryStatusAreaItemClass   BatteryStatusAreaItemClass;
typedef struct _BatteryStatusAreaItemPrivate BatteryStatusAreaItemPrivate;

struct _BatteryStatusAreaItem
{
    HDStatusMenuItem parent;
    BatteryStatusAreaItemPrivate *priv;
};

struct _BatteryStatusAreaItemClass
{
    HDStatusMenuItemClass parent_class;
};

struct _BatteryStatusAreaItemPrivate
{
    GtkWidget *title;
    GtkWidget *value;
    GtkWidget *image;
    DBusConnection *sysbus_conn;
    LibHalContext *ctx;
    ca_context *context;
    gpointer data;
    guint bme_timer;
    guint charger_timer;
    int percentage;
    int current;
    int design;
    int time;
    int bars;
    gboolean is_charging;
    gboolean is_discharging;
    gboolean verylow;
    gboolean bme_running;
    time_t bme_last_update;
};

GType battery_status_plugin_get_type (void);

G_END_DECLS

HD_DEFINE_PLUGIN_MODULE (BatteryStatusAreaItem, battery_status_plugin, HD_TYPE_STATUS_MENU_ITEM);

static void
battery_status_plugin_play_sound (BatteryStatusAreaItem *plugin, const char *file)
{
    ca_proplist *pl = NULL;
    int volume = profile_get_value_as_int (NULL, "system.sound.level");

    if (volume == 1)
        volume = -11;
    else if (volume == 2)
        volume = -1;
    else
        return;

    if (ca_proplist_create (&pl) < 0)
    {
        g_warning ("Could not create Canberra proplist");
        return;
    }

    ca_proplist_sets (pl, CA_PROP_MEDIA_NAME, "Battery Notification");
    ca_proplist_sets (pl, CA_PROP_MEDIA_FILENAME, file);
    ca_proplist_setf (pl, CA_PROP_CANBERRA_VOLUME, "%d", volume);

    ca_context_play_full (plugin->priv->context, 0, pl, NULL, NULL);

    ca_proplist_destroy (pl);
}

static void
battery_status_plugin_update_icon (BatteryStatusAreaItem *plugin, int id)
{
    const char *name;
    GdkPixbuf *pixbuf;
    GtkIconTheme *icon_theme;

    switch (id)
    {
        case -1:
            name = "statusarea_battery_verylow";
            break;
        case 0:
            name = "statusarea_battery_low";
            break;
        case 1:
            name = "statusarea_battery_full13";
            break;
        case 2:
            name = "statusarea_battery_full25";
            break;
        case 3:
            name = "statusarea_battery_full38";
            break;
        case 4:
            name = "statusarea_battery_full50";
            break;
        case 5:
            name = "statusarea_battery_full63";
            break;
        case 6:
            name = "statusarea_battery_full75";
            break;
        case 7:
            name = "statusarea_battery_full88";
            break;
        case 8:
            name = "statusarea_battery_full100";
            break;
        default:
            return;
    }

    icon_theme = gtk_icon_theme_get_default ();

    pixbuf = gtk_icon_theme_load_icon (icon_theme, name, 18, GTK_ICON_LOOKUP_NO_SVG, NULL);
    hd_status_plugin_item_set_status_area_icon (HD_STATUS_PLUGIN_ITEM (plugin), pixbuf);
    g_object_unref (pixbuf);

    pixbuf = gtk_icon_theme_load_icon (icon_theme, name, 48, GTK_ICON_LOOKUP_NO_SVG, NULL);
    gtk_image_set_from_pixbuf (GTK_IMAGE (plugin->priv->image), pixbuf);
    g_object_unref (pixbuf);
}

static void
battery_status_plugin_update_text (BatteryStatusAreaItem *plugin)
{
    int num;
    const char *str;
    gchar text[64];
    gchar text2[64];
    gchar *ptr;
    gchar *ptr2;

    ptr = text;
    ptr += g_snprintf (ptr, text+sizeof (text)-ptr, "%s: ", dgettext ("osso-dsm-ui", "tncpa_li_plugin_sb_battery"));

    if (plugin->priv->is_charging && plugin->priv->is_discharging)
        ptr += g_snprintf (ptr, text+sizeof (text)-ptr, "%s", dgettext ("osso-dsm-ui", "incf_me_battery_charged"));
    else if (!plugin->priv->is_charging)
        ptr += g_snprintf (ptr, text+sizeof (text)-ptr, "%d%%", plugin->priv->percentage);
    else
    {
        if (plugin->priv->percentage != 0)
            ptr += g_snprintf (ptr, text+sizeof (text)-ptr, "%d%% ", plugin->priv->percentage);
        ptr += g_snprintf (ptr, text+sizeof (text)-ptr, "%s", dgettext ("osso-dsm-ui", "incf_me_battery_charging"));
    }

    gtk_label_set_text (GTK_LABEL (plugin->priv->title), text);

    ptr = text;
    ptr += g_snprintf (ptr, text+sizeof (text)-ptr, "%d/%d mAh", plugin->priv->current, plugin->priv->design);

    if (!plugin->priv->is_charging || !plugin->priv->is_discharging)
    {
        if (plugin->priv->time/60/60/24 > 0)
        {
            num = plugin->priv->time/60/60/24;
            str = dngettext ("osso-clock", "cloc_va_amount_day", "cloc_va_amount_days", num);
        }
        else if (plugin->priv->time/60/60 > 0)
        {
            num = plugin->priv->time/60/60;
            str = dngettext ("mediaplayer", "mp_bd_label_hour%s", "mp_bd_label_hours%s", num);

            strncpy (text2, str, sizeof (text2)-1);
            text2[sizeof (text2)-1] = 0;
            ptr2 = strstr (text2, "%s");

            if (ptr2)
            {
                *(ptr2+1) = 'd';
                str = text2;
            }
        }
        else if (plugin->priv->time/60 > 0)
        {
            num = plugin->priv->time/60;
            str = dngettext ("osso-display", "disp_va_do_2", "disp_va_gene_2", num);
        }
        else
        {
            num = 0;
            str = NULL;
        }

        if (num && str)
        {
            ptr += g_snprintf (ptr, text+sizeof (text)-ptr, "   ");
            ptr += g_snprintf (ptr, text+sizeof (text)-ptr, str, num);
        }
    }

    gtk_label_set_text (GTK_LABEL (plugin->priv->value), text);
}

static gboolean
battery_status_plugin_charging_timeout (gpointer data)
{
    BatteryStatusAreaItem *plugin = data;
    static int id = 1;

    battery_status_plugin_update_icon (plugin, id);
    id = id%8+1; /* id is 1..8 */
    return TRUE;
}

static void
battery_status_plugin_charging_start (BatteryStatusAreaItem *plugin)
{
    hildon_banner_show_information (GTK_WIDGET (plugin), NULL, dgettext ("osso-dsm-ui", "incf_ib_battery_charging"));
    battery_status_plugin_play_sound (plugin, "/usr/share/sounds/ui-charging_started.wav");

    if (plugin->priv->charger_timer == 0)
        plugin->priv->charger_timer = g_timeout_add_seconds (1, battery_status_plugin_charging_timeout, plugin);
}

static void
battery_status_plugin_charging_stop (BatteryStatusAreaItem *plugin)
{
    if (plugin->priv->is_charging && plugin->priv->is_discharging)
        hildon_banner_show_information (GTK_WIDGET (plugin), NULL, dgettext ("osso-dsm-ui", "incf_ib_battery_full"));
    else if (!plugin->priv->is_charging)
        hildon_banner_show_information (GTK_WIDGET (plugin), NULL, dgettext ("osso-dsm-ui", "incf_ib_disconnect_charger"));

    if (plugin->priv->charger_timer > 0)
    {
        g_source_remove (plugin->priv->charger_timer);
        plugin->priv->charger_timer = 0;
    }

    battery_status_plugin_update_icon (plugin, plugin->priv->bars);
}

static void
battery_status_plugin_battery_empty (BatteryStatusAreaItem *plugin)
{
    hildon_banner_show_information_override_dnd (GTK_WIDGET (plugin), dgettext ("osso-dsm-ui", "incf_ib_battery_recharge"));
    battery_status_plugin_update_icon (plugin, -1);
    battery_status_plugin_play_sound (plugin, "/usr/share/sounds/ui-recharge_battery.wav");
}

static void
battery_status_plugin_battery_low (BatteryStatusAreaItem *plugin)
{
    hildon_banner_show_information_override_dnd (GTK_WIDGET (plugin), dgettext ("osso-dsm-ui", "incf_ib_battery_low"));
    battery_status_plugin_update_icon (plugin, 0);
    battery_status_plugin_play_sound (plugin, "/usr/share/sounds/ui-battery_low.wav");

    if (plugin->priv->verylow)
        battery_status_plugin_play_sound (plugin, "/usr/share/sounds/ui-battery_low.wav");
}

static void
battery_status_plugin_update_values (BatteryStatusAreaItem *plugin)
{
    int percentage = 0;
    int current = -1;
    int design = 0;
    int time = 0;
    int bars = 0;
    int bars_new = 0;
    gboolean verylow = FALSE;

    if (plugin->priv->bme_running && libhal_device_exists (plugin->priv->ctx, HAL_BME_UDI, NULL))
    {
        percentage = libhal_device_get_property_int (plugin->priv->ctx, HAL_BME_UDI, HAL_PERCENTAGE_KEY, NULL);
        current = libhal_device_get_property_int (plugin->priv->ctx, HAL_BME_UDI, HAL_CURRENT_KEY, NULL);
        design = libhal_device_get_property_int (plugin->priv->ctx, HAL_BME_UDI, HAL_DESIGN_KEY, NULL);
        time = libhal_device_get_property_int (plugin->priv->ctx, HAL_BME_UDI, HAL_TIME_KEY, NULL);
        bars = libhal_device_get_property_int (plugin->priv->ctx, HAL_BME_UDI, HAL_BARS_KEY, NULL);

        if (percentage == 0)
            verylow = TRUE;
    }

    if (libhal_device_exists (plugin->priv->ctx, HAL_BQ_UDI, NULL))
    {
        percentage = libhal_device_get_property_int (plugin->priv->ctx, HAL_BQ_UDI, HAL_PERCENTAGE_KEY, NULL);
        current = libhal_device_get_property_int (plugin->priv->ctx, HAL_BQ_UDI, HAL_CURRENT_KEY, NULL);
        time = libhal_device_get_property_int (plugin->priv->ctx, HAL_BQ_UDI, HAL_TIME_KEY, NULL);

        if (design == 0)
            design = libhal_device_get_property_int (plugin->priv->ctx, HAL_BQ_UDI, "battery.charge_level.last_full", NULL);

        if (current > 0 && current <= 80)
            verylow = TRUE;
        else if (current == 0)
            current = -1;
    }
    else if (!plugin->priv->bme_running)
    {
        if (plugin->priv->is_charging)
        {
            plugin->priv->is_charging = FALSE;
            battery_status_plugin_charging_stop (plugin);
        }
        plugin->priv->is_discharging = TRUE;
    }

    if (libhal_device_exists (plugin->priv->ctx, HAL_RX_UDI, NULL))
    {
        design = libhal_device_get_property_int (plugin->priv->ctx, HAL_RX_UDI, HAL_DESIGN_KEY, NULL);
        if (design > 0 && plugin->priv->design > 0 && abs (plugin->priv->design - design) < 100)
            design = plugin->priv->design;
    }

    if (current > 0 && design > 0)
        percentage = 100 * current / design;

    bars_new = 8 * (6.25 + percentage) / 100;
    if (!plugin->priv->bme_running || bars_new < bars)
        bars = bars_new;

    if (plugin->priv->verylow != verylow)
        plugin->priv->verylow = verylow;

    if (!plugin->priv->bme_running && current != -1)
    {
        if (current < 20)
            battery_status_plugin_battery_empty (plugin);
        else if (current < 200 && plugin->priv->percentage != percentage && percentage%2 == 0)
            battery_status_plugin_battery_low (plugin);
    }

    if (bars < 0)
        bars = 0;
    else if (bars > 8)
        bars = 8;

    if (percentage < 0)
        percentage = 0;

    if (current < 0)
        current = 0;

    if (design < 0)
        design = 0;

    if (time < 0)
        time = 0;

    if (plugin->priv->bars != bars)
    {
        plugin->priv->bars = bars;
        if (plugin->priv->is_discharging)
            battery_status_plugin_update_icon (plugin, bars);
    }

    plugin->priv->percentage = percentage;
    plugin->priv->current = current;
    plugin->priv->design = design;
    plugin->priv->time = time;

    battery_status_plugin_update_text (plugin);
}

static void
battery_status_plugin_update_charging (BatteryStatusAreaItem *plugin, const char *udi)
{
    gboolean is_charging;
    gboolean is_discharging;

    is_charging = libhal_device_get_property_bool (plugin->priv->ctx, udi, HAL_IS_CHARGING_KEY, NULL);
    is_discharging = libhal_device_get_property_bool (plugin->priv->ctx, udi, HAL_IS_DISCHARGING_KEY, NULL);

    if (plugin->priv->is_charging != is_charging || plugin->priv->is_discharging != is_discharging)
    {
        if (!is_charging && !is_discharging)
            is_discharging = TRUE;

        plugin->priv->is_charging = is_charging;
        plugin->priv->is_discharging = is_discharging;

        if (is_charging && !is_discharging)
            battery_status_plugin_charging_start (plugin);
        else if (is_discharging)
            battery_status_plugin_charging_stop (plugin);
    }
}

static gboolean
battery_status_plugin_bme_process_timeout (gpointer data)
{
    BatteryStatusAreaItem *plugin = data;
    gboolean bme_running = system ("pgrep -f ^/usr/sbin/bme_RX-51 1>/dev/null 2>&1") == 0;

    if (plugin->priv->bme_running != bme_running)
    {
        if (bme_running)
            battery_status_plugin_update_charging (plugin, HAL_BME_UDI);

        plugin->priv->bme_running = bme_running;
        battery_status_plugin_update_values (plugin);
    }

    return TRUE;
}

static void
battery_status_plugin_hal_device_modified_cb (LibHalContext *ctx, const char *udi)
{
    BatteryStatusAreaItem *plugin = libhal_ctx_get_user_data (ctx);

    if (strcmp (udi, HAL_BQ_UDI) != 0 && strcmp (udi, HAL_RX_UDI) != 0 && strcmp (udi, HAL_BME_UDI) != 0)
        return;

    if (strcmp (udi, HAL_RX_UDI) == 0)
         plugin->priv->design = 0;

    if (libhal_device_exists (plugin->priv->ctx, udi, NULL))
        battery_status_plugin_update_charging (plugin, udi);

    battery_status_plugin_update_values (plugin);
}

static void
battery_status_plugin_hal_property_modified_cb (LibHalContext *ctx, const char *udi, const char *key, dbus_bool_t is_removed G_GNUC_UNUSED, dbus_bool_t is_added G_GNUC_UNUSED)
{
    BatteryStatusAreaItem *plugin = libhal_ctx_get_user_data (ctx);
    const char *str;

    if (strcmp (udi, HAL_BQ_UDI) != 0 && strcmp (udi, HAL_RX_UDI) != 0 && strcmp (udi, HAL_BME_UDI) != 0)
        return;

    if (strcmp (udi, HAL_BME_UDI) == 0)
    {
        if (!plugin->priv->bme_running)
            battery_status_plugin_bme_process_timeout (plugin);

        if (strcmp (key, HAL_CAPACITY_KEY) == 0)
        {
            str = libhal_device_get_property_string (plugin->priv->ctx, udi, key, NULL);
            if (strcmp (str, "empty") == 0)
                battery_status_plugin_battery_empty (plugin);
            else if (strcmp (str, "low") == 0)
                battery_status_plugin_battery_low (plugin);
            else if (strcmp (str, "full") == 0)
                battery_status_plugin_update_icon (plugin, 8);
        }

        if (strcmp (key, HAL_IS_CHARGING_KEY) == 0 || strcmp (key, HAL_IS_DISCHARGING_KEY) == 0)
            plugin->priv->bme_last_update = time(NULL);
    }

    if (strcmp (udi, HAL_BME_UDI) == 0 || plugin->priv->bme_last_update + 15 < time(NULL))
        if (strcmp (key, HAL_IS_CHARGING_KEY) == 0 || strcmp (key, HAL_IS_DISCHARGING_KEY) == 0)
            battery_status_plugin_update_charging (plugin, udi);

    battery_status_plugin_update_values (plugin);
}

static void
battery_status_plugin_init (BatteryStatusAreaItem *plugin)
{
    DBusError error;
    gboolean bme_replacement;
    GtkWidget *alignment;
    GtkWidget *hbox;
    GtkWidget *label_box;
    GtkStyle *style;

    plugin->priv = G_TYPE_INSTANCE_GET_PRIVATE (plugin, battery_status_plugin_get_type (), BatteryStatusAreaItemPrivate);

    dbus_error_init (&error);

    plugin->priv->sysbus_conn = dbus_bus_get (DBUS_BUS_SYSTEM, &error);
    if (dbus_error_is_set (&error))
    {
        g_warning ("Could not open D-Bus session bus connection");
        dbus_error_free (&error);
        return;
    }

    dbus_error_free (&error);

    plugin->priv->ctx = libhal_ctx_new ();
    if (!plugin->priv->ctx)
    {
        g_warning ("Could not open HAL context");
        return;
    }

    libhal_ctx_set_dbus_connection (plugin->priv->ctx, plugin->priv->sysbus_conn);
    libhal_ctx_set_user_data (plugin->priv->ctx, plugin);
    libhal_ctx_set_device_added (plugin->priv->ctx, battery_status_plugin_hal_device_modified_cb);
    libhal_ctx_set_device_removed (plugin->priv->ctx, battery_status_plugin_hal_device_modified_cb);
    libhal_ctx_set_device_property_modified (plugin->priv->ctx, battery_status_plugin_hal_property_modified_cb);
    libhal_ctx_init (plugin->priv->ctx, NULL);
    libhal_device_add_property_watch (plugin->priv->ctx, HAL_BQ_UDI, NULL);
    libhal_device_add_property_watch (plugin->priv->ctx, HAL_RX_UDI, NULL);
    libhal_device_add_property_watch (plugin->priv->ctx, HAL_BME_UDI, NULL);

    plugin->priv->context = NULL;
    if (ca_context_create (&plugin->priv->context) < 0)
    {
        g_warning ("Could not create Canberra context");
        return;
    }

    ca_context_open (plugin->priv->context);

    plugin->priv->title = gtk_label_new (NULL);
    if (!plugin->priv->title)
    {
        g_warning ("Could not create GtkLabel");
        return;
    }

    plugin->priv->value = gtk_label_new (NULL);
    if (!plugin->priv->value)
    {
        g_warning ("Could not create GtkLabel");
        gtk_widget_destroy (plugin->priv->title);
        return;
    }

    plugin->priv->image = gtk_image_new ();
    if (!plugin->priv->value)
    {
        g_warning ("Could not create GtkImage");
        gtk_widget_destroy (plugin->priv->title);
        gtk_widget_destroy (plugin->priv->value);
        return;
    }

    alignment = gtk_alignment_new (0, 0.5, 0, 0);
    if (!alignment)
    {
        g_warning ("Could not create GtkAlignment");
        gtk_widget_destroy (plugin->priv->title);
        gtk_widget_destroy (plugin->priv->value);
        gtk_widget_destroy (plugin->priv->image);
        return;
    }

    hbox = gtk_hbox_new (FALSE, 0);
    if (!hbox)
    {
        g_warning ("Could not create GtkHBox");
        gtk_widget_destroy (plugin->priv->title);
        gtk_widget_destroy (plugin->priv->value);
        gtk_widget_destroy (plugin->priv->image);
        gtk_widget_destroy (alignment);
        return;
    }

    label_box = gtk_vbox_new (FALSE, 0);
    if (!hbox)
    {
        g_warning ("Could not create GtkVBox");
        gtk_widget_destroy (plugin->priv->title);
        gtk_widget_destroy (plugin->priv->value);
        gtk_widget_destroy (plugin->priv->image);
        gtk_widget_destroy (alignment);
        gtk_widget_destroy (hbox);
        return;
    }

    gtk_widget_set_name (plugin->priv->title, "hildon-button-title");
    gtk_widget_set_name (plugin->priv->value, "hildon-button-value");

    gtk_misc_set_alignment (GTK_MISC (plugin->priv->title), 0, 0.5);
    gtk_misc_set_alignment (GTK_MISC (plugin->priv->value), 0, 0.5);
    gtk_misc_set_alignment (GTK_MISC (plugin->priv->image), 0.5, 0.5);

    style = gtk_rc_get_style_by_paths (gtk_settings_get_default (), "SmallSystemFont", NULL, G_TYPE_NONE);
    if (style && style->font_desc)
        gtk_widget_modify_font (GTK_WIDGET (plugin->priv->value), style->font_desc);

    gtk_box_pack_start (GTK_BOX (label_box), plugin->priv->title, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (label_box), plugin->priv->value, TRUE, TRUE, 0);

    gtk_box_pack_start (GTK_BOX (hbox), plugin->priv->image, FALSE, FALSE, 0);
    gtk_box_pack_start (GTK_BOX (hbox), label_box, TRUE, TRUE, 0);

    gtk_container_add (GTK_CONTAINER (alignment), hbox);

    plugin->priv->is_discharging = TRUE;
    plugin->priv->bme_running = TRUE;

    bme_replacement = system ("dpkg -l bme-rx-51 2>/dev/null | grep -q '^ii' && dpkg --compare-versions \"$(dpkg-query -W -f \\${Version} bme-rx-51)\" ge '1.0'") == 0;
    if (!bme_replacement)
        plugin->priv->bme_timer = g_timeout_add_seconds (30, battery_status_plugin_bme_process_timeout, plugin);

    if (libhal_device_exists (plugin->priv->ctx, HAL_BME_UDI, NULL))
        battery_status_plugin_update_charging (plugin, HAL_BME_UDI);

    if (libhal_device_exists (plugin->priv->ctx, HAL_BQ_UDI, NULL))
        battery_status_plugin_update_charging (plugin, HAL_BQ_UDI);

    battery_status_plugin_update_icon (plugin, 0);
    battery_status_plugin_update_text (plugin);
    battery_status_plugin_bme_process_timeout (plugin);
    battery_status_plugin_update_values (plugin);

    gtk_container_add (GTK_CONTAINER (plugin), alignment);
    gtk_widget_show_all (GTK_WIDGET (plugin));

}

static void
battery_status_plugin_finalize (GObject *object)
{
    BatteryStatusAreaItem *plugin = G_TYPE_CHECK_INSTANCE_CAST (object, battery_status_plugin_get_type (), BatteryStatusAreaItem);

    if (plugin->priv->context)
    {
        ca_context_destroy (plugin->priv->context);
        plugin->priv->context = NULL;
    }

    if (plugin->priv->ctx)
    {
        libhal_ctx_shutdown (plugin->priv->ctx, NULL);
        libhal_ctx_free (plugin->priv->ctx);
        plugin->priv->ctx = NULL;
    }

    if (plugin->priv->sysbus_conn)
    {
        dbus_connection_unref (plugin->priv->sysbus_conn);
        plugin->priv->sysbus_conn = NULL;
    }

    if (plugin->priv->bme_timer > 0)
    {
        g_source_remove (plugin->priv->bme_timer);
        plugin->priv->bme_timer = 0;
    }

    if (plugin->priv->charger_timer > 0)
    {
        g_source_remove (plugin->priv->charger_timer);
        plugin->priv->charger_timer = 0;
    }

    G_OBJECT_CLASS (battery_status_plugin_parent_class)->finalize (object);
}

static void
battery_status_plugin_class_init (BatteryStatusAreaItemClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    object_class->finalize = (GObjectFinalizeFunc) battery_status_plugin_finalize;

    g_type_class_add_private (klass, sizeof (BatteryStatusAreaItemPrivate));
}

static void
battery_status_plugin_class_finalize (BatteryStatusAreaItemClass *klass G_GNUC_UNUSED)
{
}
