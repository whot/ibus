/* gcc -Wall -o uinputter uinputter.c $(pkg-config --cflags --libs libevdev) */

#include <glib.h>

#include <errno.h>
#include <grp.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

#ifdef LIBSYSTEMD_SHARED
/* Needs a private library libsystemd-shared-257.7-1.fc42.so
 * uinput_replay_LDADD = -L$(libdir)/systemd -lsystemd-shared-257.7-1.fc42
 * uinput_replay_LDFLAGS = -Wl,-rpath,$(libdir)/systemd
 */
#include <systemd/sd-device.h>
#include <systemd/sd-event.h>
#endif /* end of LIBSYSTEMD_SHARED */

#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define msleep(t) usleep((t) * 1000)

#ifdef LIBSYSTEMD_SHARED
#define STRNA(s) (s)?: "N/A"

/* systemd/src/libsystemd/sd-device/device-monitor-private.h */
typedef enum MonitorNetlinkGroup {
        MONITOR_GROUP_NONE,
        MONITOR_GROUP_KERNEL,
        MONITOR_GROUP_UDEV,
        _MONITOR_NETLINK_GROUP_MAX,
        _MONITOR_NETLINK_GROUP_INVALID = -EINVAL,
} MonitorNetlinkGroup;

extern int device_monitor_new_full (sd_device_monitor **ret,
                                    MonitorNetlinkGroup group,
                                    int                 fd);

/* systemd/src/libsystemd/sd-device/device-private.c:
 * DEFINE_STRING_TABLE_LOOKUP() in %{libdir}/systemd/libsystemd-shared*.so
 */
extern const char* device_action_to_string (sd_device_action_t i);

static pid_t m_pid_udev_monitor;
#endif /* end of LIBSYSTEMD_SHARED */

static gchar *
get_case_contents (const gchar *case_path)
{
    gchar *contents = NULL;
    gsize length = 0; 
    GError *error = NULL;
 
    g_return_val_if_fail (case_path, NULL);
    if (!g_file_get_contents (case_path, &contents, &length, &error)) {
        g_warning ("Failed to open %s: %s", case_path, error->message);
        g_error_free (error);
    } else if (length == 0) { 
        g_warning ("No contents file %s", case_path);
    }   
    return contents;
}


#ifdef LIBSYSTEMD_SHARED
static int
device_monitor_handler (sd_device_monitor *monitor,
                        sd_device         *device,
                        void              *userdata)
{
    sd_event *event = (sd_event *)userdata;
    sd_device_action_t action = _SD_DEVICE_ACTION_INVALID;
    const char *devpath = NULL, *subsystem = NULL;

    sd_device_get_action (device, &action);
    sd_device_get_devpath (device, &devpath);
    sd_device_get_subsystem (device, &subsystem);

    g_message ("%-8s %s (%s)",
               STRNA (device_action_to_string (action)),
               devpath, subsystem);
    if (g_str_has_prefix (STRNA (device_action_to_string (action)), "add") &&
        g_str_has_prefix (g_path_get_basename (devpath), "event") &&
        !g_strcmp0 (subsystem, "input")) {
        sd_event_exit (event, 0);
    }
    return 0;
}


static int
setup_monitor (MonitorNetlinkGroup sender,
               sd_event           *event,
               sd_device_monitor **ret) {
    sd_device_monitor *monitor = NULL;

    if (device_monitor_new_full (&monitor, sender, -1) < 0) {
        g_warning ("Failed to create netlink socket: %s", g_strerror (errno));
        return -1;
    }
    if (sd_device_monitor_attach_event (monitor, event) < 0) {
        g_warning ("Failed to attach event: %s", g_strerror (errno));
        return -1;
    }
    if (sd_device_monitor_start (monitor, device_monitor_handler, event) < 0) {
        g_warning ("Failed to attach event: %s", g_strerror (errno));
        return -1;
    }
    *ret = monitor;
    monitor = NULL;
    return 0;
}


/* Refer systemd/src/udev/udevadm-monitor.c */
static gboolean
setup_udev_monitor (void)
{
    sd_device_monitor *udev_monitor = NULL;
    sd_event *event = NULL;

    errno = 0;
    if (sd_event_default(&event) < 0) {
        g_warning ("Failed to initialize sd_event: %s", g_strerror (errno));
        sd_event_unrefp (&event);
        return FALSE;
    }
    if (sd_event_set_signal_exit (event, TRUE) < 0) {
        g_warning ("Failed to install SIGINT/SIGTERM handling: %s",
                   g_strerror (errno));
        sd_event_unrefp (&event);
        return FALSE;
    }
    if (setup_monitor (MONITOR_GROUP_UDEV, event, &udev_monitor) < 0) {
        g_warning ("Failed to udev monitor: %s",
                   g_strerror (errno));
        sd_event_unrefp (&event);
        return FALSE;
    }
    if (sd_event_loop (event) < 0) {
        g_warning ("Failed to run udev event: %s", g_strerror (errno));
        sd_device_monitor_unrefp (&udev_monitor);
        sd_event_unrefp (&event);
        return FALSE;
    }
    sd_device_monitor_unrefp (&udev_monitor);
    sd_event_unrefp (&event);
    return TRUE;
}
#endif /* end of LIBSYSTEMD_SHARED */


static struct libevdev_uinput *
ibus_uidev_new (void)
{
    struct libevdev *dev = libevdev_new ();
    unsigned int code;
    struct libevdev_uinput *uidev = NULL;
    int retval;
    const char *syspath;
    const char *devnode;
    struct stat buf;
    struct group *grp = NULL;

#ifdef LIBSYSTEMD_SHARED
    if (!(m_pid_udev_monitor = fork ())) {
        exit (!setup_udev_monitor ());
    }
#endif
    libevdev_set_name (dev, "ibusdev");
    /* serial makes it an internal keyboard */
    libevdev_set_id_bustype (dev, BUS_I8042);

    for (code = KEY_ESC; code < BTN_MISC; code++)
        libevdev_enable_event_code (dev, EV_KEY, code, NULL);

    retval = libevdev_uinput_create_from_device (
            dev, 
            LIBEVDEV_UINPUT_OPEN_MANAGED,
            &uidev);
    libevdev_free (dev);

    if (retval) {
        g_warning ("Failed to create uinput: %s\n", g_strerror (-retval));
        return NULL;
    }

    syspath = libevdev_uinput_get_syspath (uidev);
    g_assert (syspath != NULL);
    devnode = libevdev_uinput_get_devnode (uidev);
    g_assert (devnode != NULL);

    /* You need to wait here until the device actually exists, either via a
     * sleep or checking udev until the device shows up. Use
     * libevdev_uinput_get_syspath() for the latter
     */
    errno = 0;
    do {
        msleep (10);
        if (stat (devnode, &buf)) {
            g_warning ("Failed to get stat of %s: %s\n",
                       devnode, g_strerror (errno));
            libevdev_uinput_destroy (uidev);
            return NULL;
        }
        if (!(grp = getgrgid (buf.st_gid))) {
            g_warning ("Failed to get gid of %s: %s\n",
                       devnode, g_strerror (errno));
            libevdev_uinput_destroy (uidev);
            return NULL;
        }
        g_debug ("gr_name %s", grp->gr_name);
    } while (strcmp (grp->gr_name, "input"));
    msleep (100);

    return uidev;
}


static gboolean
parse_array (const gchar *line, 
             guint       *array)
{
    const gchar *head = line;
    int array_index = 0;

    g_assert (line);
    g_assert (array);

    while (*head != '\0') {
        gchar *end = NULL;
        array[array_index] = g_ascii_strtoull (head, &end, 10);
        while (*end != ',' && *end != ']' && *end != '\0') ++end;
        if (*end == ',') {
            ++array_index;
            head = end + 1;
        } else if (*end == ']') {
            if (array_index == 4) {
                return TRUE;
            } else {
                g_warning ("Fewer array index %d < 4", array_index);
                return FALSE;
            }
        } else {
            g_warning ("array does not enclose with ']'");
            return FALSE;
        }
    }
    return FALSE;
}


static gboolean
parse_evdev (const gchar            *line,
             guint                  *start_events,
             guint                  *start_evdev,
             guint                  *start_keys,
             guint                  *array)
{
    const gchar *p = line;
    guint indent;

    g_assert (line);
    g_assert (start_events);
    g_assert (start_evdev);
    g_assert (start_keys);

    while (*p == ' ') ++p;
    if (*p == '-') {
        ++p;
        while (*p == ' ') ++p;
    }
    if (*p == '#' || *p == '\0')
        return FALSE;
    indent = p - line;
    if (*start_events == 0 && !g_ascii_strncasecmp (p, "events", 6)) {
        *start_events = indent;
    } else if (*start_events > 0 && indent > *start_events  &&
               !g_ascii_strncasecmp (p, "evdev", 5)) {
        *start_evdev = indent;
    } else if (*start_events > 0 && *start_evdev > 0) {
        if (*p == '[') {
            if (*start_keys == 0) {
                *start_keys = indent;
            } else if (*start_keys != indent) {
                g_warning ("Wrong indent");
                *start_events = 0;
                *start_evdev = 0;
                *start_keys = 0;
                return FALSE;
            }
            if (parse_array (p + 1, array))
                return TRUE;
        } else {
            *start_events = 0;
            *start_evdev = 0;
            *start_keys = 0;
        }
    } else {
        *start_events = 0;
        *start_evdev = 0;
        *start_keys = 0;
    }
    return FALSE;
}


static gboolean
ibus_uidev_write_event_array (struct libevdev_uinput *uidev,
                              guint                  *array)
{
    static guint prev_time = 0;
    guint interval;
    int retval;

    g_assert (array);
    g_assert (array[1] >= prev_time);
    interval = array[1] - prev_time;
    prev_time = array[1];
    if (interval > 0) {
        g_debug ("usleep %u\n", interval);
        usleep(interval);
    }
    g_debug ("write uinput(%u, %u, %u, %u, %u)\n",
             array[0], array[1], array[2], array[3], array[4]);
    retval = libevdev_uinput_write_event (uidev, array[2], array[3], array[4]);
    if (retval) {
        g_warning ("Failed to write uinput(%u, %u, %u, %u, %u): %s",
                   array[0], array[1], array[2], array[3], array[4],
                   g_strerror (-retval));
        return FALSE;
    }
    return TRUE;
}


static void
ibus_uidev_replay_with_yaml_data (struct libevdev_uinput *uidev,
                                  const gchar            *contents)
{
    const gchar *head = contents;
    guint start_events = 0;
    guint start_evdev = 0;
    guint start_keys = 0;

    g_assert (uidev);
    g_assert (contents);
    while (*head == '\n') ++head;
    while (*head != '\0') {
        const gchar *end = head;
        gchar *line;
        guint array[5] = { 0, };

        while (*end != '\n' && *end != '\0') ++end;
        if (*head == '#') {
            head = end;
            if (*head != '\n' && *head != '\0')
                ++head;
            continue;
        }
        line = g_strndup (head, end - head);
        if (parse_evdev (line, &start_events, &start_evdev, &start_keys, array))
            if (!ibus_uidev_write_event_array (uidev, array))
                return;
        g_free (line);
        head = end;
        if (*head != '\0')
            ++head;
    }
}

int
main (int argc, char *argv[]) {
    gchar *contents;
    struct libevdev_uinput *uidev;

    if (argc == 1) {
        gchar *prgname = g_path_get_basename (argv[0]);
        g_warning ("Usage: %s libinput-test.yml", prgname);
        g_free (prgname);
        return EXIT_FAILURE;
    }

    if (!g_strcmp0 (argv[1], "--username")) {
        g_print ("%s\n", g_get_user_name ());
        return EXIT_SUCCESS;
    }

    if (!(contents = get_case_contents (argv[1])))
        return EXIT_FAILURE;
    if (!(uidev = ibus_uidev_new ()))
        return EXIT_FAILURE;

    ibus_uidev_replay_with_yaml_data (uidev, contents);
   
    g_free (contents);
    libevdev_uinput_destroy (uidev);

    return EXIT_SUCCESS;
}
