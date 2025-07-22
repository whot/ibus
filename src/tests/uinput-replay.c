/* gcc -Wall -o uinputter uinputter.c $(pkg-config --cflags --libs libevdev) */

#include <glib.h>

#include <errno.h>
#include <grp.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>
#include <libudev.h>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define msleep(t) usleep((t) * 1000)

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

static struct udev_monitor *
udev_setup_monitor(void)
{
	struct udev *udev;
	struct udev_monitor *udev_monitor;
	int rc;

	udev = udev_new ();
	if (!udev) {
        g_warning ("Failed to create udev context");
        return NULL;
    }
	udev_monitor = udev_monitor_new_from_netlink (udev, "udev");
	if (!udev_monitor) {
        g_warning ("Failed to create udev context");
        goto out;
    }
	udev_monitor_filter_add_match_subsystem_devtype (udev_monitor, "input", NULL);

	/* remove O_NONBLOCK */
	rc = fcntl (udev_monitor_get_fd (udev_monitor), F_SETFL, 0);
	if (rc == -1) {
        g_warning ("Failed to remove O_NONBLOCK on udev monitor: %s", strerror (errno));
        goto out;
    }
	rc = udev_monitor_enable_receiving (udev_monitor);
    if (rc < 0) {
        g_warning ("Failed enable receiving on udev monitor: %s", strerror (-rc));
        goto out;
    }

    return udev_monitor;

out:
    if (udev_monitor) {
        udev_monitor_unref (udev_monitor);
    }
	udev_unref(udev);

	return NULL;
}

static struct udev_device *
udev_wait_for_device_event(struct udev_monitor *udev_monitor,
                           const char *udev_event,
                           const char *syspath)
{
	struct udev_device *udev_device = NULL;

	/* blocking, we don't want to continue until udev is ready */
	while (1) {
		const char *udev_syspath = NULL;
		const char *udev_action;

		udev_device = udev_monitor_receive_device (udev_monitor);
		if (!udev_device) {
		    g_warning ("Failed to receive device");
		    return NULL;
        }
		udev_action = udev_device_get_action (udev_device);
		if (!udev_action || !g_str_equal (udev_action, udev_event)) {
			udev_device_unref (udev_device);
			continue;
		}

		udev_syspath = udev_device_get_syspath (udev_device);
		if (udev_syspath && g_str_has_prefix (udev_syspath, syspath))
			break;

		udev_device_unref (udev_device);
	}

	return udev_device;
}


static struct libevdev_uinput *
ibus_uidev_new (void)
{
    struct libevdev *dev = libevdev_new ();
    unsigned int code;
    struct libevdev_uinput *uidev = NULL;
    int retval;
    char *syspath;
    struct udev_monitor *udev_monitor = NULL;

    udev_monitor = udev_setup_monitor();
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
        udev_monitor_unref (udev_monitor);
        return NULL;
    }

    syspath = g_strdup_printf("%s/event", libevdev_uinput_get_syspath (uidev));
    udev_wait_for_device_event (udev_monitor, "add", syspath);
    g_free (syspath);

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
