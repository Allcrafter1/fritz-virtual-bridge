// SPDX-License-Identifier: MIT OR Apache-2.0
/* Supported only for the fingerprinted FRITZ!OS 8.25 ARM aha build. Preload
 * into aha, never other daemons. Reuses the genuine Nexus server transport.
 * Dynamic devices are managed through a mode-0600 Unix SEQPACKET control
 * socket. The fixed laboratory endpoints remain available only when the
 * explicit AHA_VIRTUAL_LEGACY=1 self-test mode is selected.
 */
#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <dlfcn.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include "device_registry.h"
#include "provider_state.h"
#include "provider_events.h"
#include "provider_codec.h"
#include "provider_profiles.h"
#include "control_parse.h"
#include "provider_transport.h"

static ssize_t (*original_write)(int, const void*, size_t);
static ssize_t (*original_send)(int, const void*, size_t, int);
static int (*original_socketpair)(int, int, int, int[2]);
static int (*original_close)(int);
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int enabled, pairs[32][2], pair_count, incoming_fd = -1;
static unsigned generation, announced;
static unsigned command_count, last_command;
static int event_pipe[2] = {
    -1, -1
};
static struct provider_event_queue event_queue;
static int debug_enabled, hanfun_experiment;
/* All registry and device state access is protected by lock. Encoders use
 * state directly: selecting a device never copies another device's values. */
static int legacy_enabled;
static fvb_device_registry registry;
static const fvb_device *selected_device;
static struct provider_state legacy_state = PROVIDER_STATE_INITIALIZER;
static struct provider_state initial_values;
static struct provider_state device_values[FVB_REGISTRY_CAPACITY];
static ssize_t emit_write(int fd, const void *buf, size_t n);
static struct provider_encoder encoder = {
    .state = &legacy_state,
    .fd = -1,
    .write = emit_write,
};
static unsigned unit_generation[FVB_REGISTRY_CAPACITY];

static void select_device(const fvb_device *device) {
    selected_device = device;
    encoder.state = &device_values[device - registry.devices];
}

static void unselect_device(void) {
    selected_device = NULL;
    encoder.state = &legacy_state;
}

static void provider_unlock(void) {
    unselect_device();
    pthread_mutex_unlock(&lock);
}

static unsigned profile_remote(fvb_device_profile p) {
    static const unsigned ids[] = {
        450, 452, 453, 454, 455
    };
    return ids[p];
}

#define DEBUG(...) do{if(debug_enabled)fprintf(stderr,__VA_ARGS__);}while(0)
static const char *control_path = "/var/tmp/aha-virtual-provider.ctl";
/* Encoder output uses the selected immutable identity. */
static ssize_t emit_write(int fd, const void *buf, size_t n) {
    unsigned char packet[512];
    if (!selected_device) return provider_write_frame(fd, buf, n, original_write);
    if (n > sizeof(packet) || n < 16) return -1;
    memcpy(packet, buf, n);
    write_be16(packet+8, selected_device->remote_id);
    if (packet[0] == 4 && n == 168) {
        memset(packet+20, 0, 80);
        memcpy(packet+20, selected_device->name, strlen(selected_device->name));
        memset(packet+112, 0, 20);
        memcpy(packet+112, selected_device->uid, 19);
    }
    else if (packet[0] == 7 && n >= 24) {
        unsigned function = read_be32(packet+16);
        if (function == 95 && n == 48) {
            memcpy(packet+24, selected_device->hanfun.ipui, 5);
            write_be16(packet+32, selected_device->hanfun.discriminator);
        }
        if (function == 98 && n == 216) {
            memset(packet+24, 0, 80);
            memcpy(packet+24, selected_device->name, strlen(selected_device->name));
            /* The local 8.25 ETSI receiver swaps this opaque interface array
             * again after decoding the network payload. */
            for (unsigned offset = 112; offset < 152; offset += 4) {
                unsigned value = read_be32(packet+offset);
                packet[offset] = value;
                packet[offset+1] = value>>8;
                packet[offset+2] = value>>16;
                packet[offset+3] = value>>24;
            }
            write_be16(packet+152, selected_device->hanfun.unit_id);
        }
        if (function == 118 && n >= 36) write_be16(packet+24, selected_device->hanfun.unit_id);
    }
    return provider_write_frame(fd, packet, n, original_write);
}
/* Lock held throughout frame encoding and delivery; frames cannot interleave. */
static const char *endpoint_for(unsigned remote_id) {
    if (remote_id == 455) return "VIRT000000000006";
    if (remote_id == 454) return "VIRT000000000005";
    if (remote_id == 453) return "VIRT000000000004";
    if (remote_id == 452) return "VIRT000000000003";
    return remote_id == 451 ? "VIRT000000000002":"VIRT000000000001";
}

static int emit_legacy_all(void) {
    int ok = emit_switch_config(&encoder) && emit_options(&encoder, 450) && emit_relay(&encoder, 450,
        encoder.state->switch_state) &&
    emit_light_config(&encoder) && emit_options(&encoder, 451) && emit_light_modes(&encoder) &&
    emit_relay(&encoder, 451, encoder.state->light_state) && emit_light_level(&encoder);
    /* Unit creation and initial status are deliberately separate control
     * operations.  This lets the operator verify that Function95 was
     * accepted before Function98, and that the Unit exists before sending
     * Function118 status traffic. */
    if (ok && hanfun_experiment)
    ok = emit_hanfun_light_config(&encoder) && emit_hanfun_device_config(&encoder) &&
    emit_color_light_config(&encoder) && emit_color_device_config(&encoder) &&
    emit_cover_config(&encoder) && emit_cover_device_config(&encoder) &&
    emit_thermostat_config(&encoder) && emit_thermostat_status(&encoder);
    return ok;
}
/* These functions are called with lock held. UNIT remains an explicit second
 * provisioning step: enqueueing Function95 does not prove parent acceptance. */
static int dynamic_status(const fvb_device *d) {
    switch (d->profile) {
        case FVB_PROFILE_SWITCH:return emit_relay(&encoder, 450, encoder.state->switch_state);
        case FVB_PROFILE_DIMMABLE_LIGHT:return emit_hanfun_relay(&encoder) && emit_hanfun_level(&encoder);
        case FVB_PROFILE_COLOR_TEMPERATURE_LIGHT:return emit_color_status(&encoder);
        case FVB_PROFILE_COVER:return emit_cover_status(&encoder);
        case FVB_PROFILE_THERMOSTAT:return emit_thermostat_status(&encoder);
        default:return 0;
    }
}

static int dynamic_announce(const fvb_device *d) {
    uint32_t now = (uint32_t) time(NULL);
    if (encoder.state->hanfun_timestamp == UINT32_MAX) return 0;
    encoder.state->hanfun_timestamp = now > encoder.state->hanfun_timestamp ? now:encoder.state->hanfun_timestamp+1;
    unit_generation[d-registry.devices] = 0;
    switch (d->profile) {
        case FVB_PROFILE_SWITCH:return emit_switch_config(&encoder) && emit_options(&encoder, 450) && dynamic_status(d);
        case FVB_PROFILE_DIMMABLE_LIGHT:return emit_hanfun_light_config(&encoder) && emit_hanfun_device_config(&encoder);
        case FVB_PROFILE_COLOR_TEMPERATURE_LIGHT:return emit_color_light_config(&encoder) && emit_color_device_config(&encoder);
        case FVB_PROFILE_COVER:return emit_cover_config(&encoder) && emit_cover_device_config(&encoder);
        case FVB_PROFILE_THERMOSTAT:return emit_thermostat_config(&encoder) && dynamic_status(d);
        default:return 0;
    }
}

static int dynamic_unit(const fvb_device *d) {
    int ok = 0;
    if (encoder.fd < 0) return 0;
    if (encoder.state->hanfun_timestamp == UINT32_MAX) return 0;
    ++encoder.state->hanfun_timestamp;
    switch (d->profile) {
        case FVB_PROFILE_DIMMABLE_LIGHT:ok = emit_hanfun_unit_config(&encoder);
        break;
        case FVB_PROFILE_COLOR_TEMPERATURE_LIGHT:ok = emit_color_unit_config(&encoder);
        break;
        case FVB_PROFILE_COVER:ok = emit_cover_unit_config(&encoder);
        break;
        default:return 0;
    }
    if (ok) {
        unit_generation[d-registry.devices] = generation;
        ok = dynamic_status(d);
    }
    return ok;
}

static int emit_all(void) {
    int ok = legacy_enabled ? emit_legacy_all():1;
    for (size_t i = 0; i < registry.count; ++i) {
        const fvb_device *d = fvb_registry_at(&registry, i);
        if (d->enabled) {
            select_device(d);
            int result = dynamic_announce(d);
            unselect_device();
            if (!result) ok = 0;
        }
    }
    return ok;
}
/* >=0 handled; -1 dispatch compatible feedback through the existing parser.
 * cmd is rewritten only after profile and enabled-state validation. */
static int dynamic_control(char *cmd, size_t capacity) {
    char operation[24], uid[20], rest[160];
    int offset = 0;
    if (sscanf(cmd, "%23s %19s %n", operation, uid, &offset) < 2) return -1;
    if (!strcmp(operation, "ADD") || !strcmp(operation, "RESTORE")) {
        unsigned restored_id = 0;
        if (!strcmp(operation, "RESTORE")) {
            char *end = NULL;
            unsigned long value = strtoul(cmd+offset, &end, 10);
            if (end == cmd+offset || *end != ' ' || value < FVB_FIRST_REMOTE_ID || value > UINT16_MAX) return 0;
            restored_id = (unsigned) value;
            offset = (int)(end-cmd)+1;
        }
        char profile[40];
        int name_offset = 0;
        if (sscanf(cmd+offset, "%39s %n", profile, &name_offset) != 1 || !name_offset) return 0;
        fvb_device_profile p;
        for (p = 0; p < FVB_PROFILE_COUNT; ++p) if (!strcmp(profile, fvb_profile_name(p))) break;
        const fvb_device *existing = fvb_registry_find(&registry, uid);
        if (existing) return (!restored_id || existing->remote_id == restored_id) && existing->profile == p && !strcmp(existing->name,
            cmd+offset+name_offset);
        uint16_t id;
        uint32_t watermark = registry.next_remote_id;
        if (restored_id) {
            if (fvb_registry_find_remote(&registry, (uint16_t) restored_id)) return 0;
            registry.next_remote_id = restored_id;
        }
        fvb_registry_result added = fvb_registry_add(&registry, registry.revision, uid, cmd+offset+name_offset,
            p, &id);
        if (added != FVB_REGISTRY_OK) {
            registry.next_remote_id = watermark;
            return 0;
        }
        if (registry.next_remote_id < watermark) registry.next_remote_id = watermark;
        const fvb_device *d = fvb_registry_find_remote(&registry, id);
        device_values[d-registry.devices] = initial_values;
        /* Configuration is accepted offline; ANNOUNCE/transport reconnect retries it. */
        if (encoder.fd >= 0) {
            select_device(d);
                (void) dynamic_announce(d);
            unselect_device();
        }
        return 1;
    }
    int management = !strcmp(operation, "RENAME") || !strcmp(operation, "ENABLE") ||
    !strcmp(operation, "DISABLE") || !strcmp(operation, "REMOVE") ||
    !strcmp(operation, "UNIT") || !strcmp(operation, "ANNOUNCE");
    if (!fvb_registry_valid_uid(uid)) return management ? 0:-1;
    const fvb_device *d = fvb_registry_find(&registry, uid);
    /* REMOVE is deliberately idempotent so an interrupted prune can be retried. */
    if (!d) return !strcmp(operation, "REMOVE");
    if (!strcmp(operation, "RENAME")) {
        if (fvb_registry_rename(&registry, registry.revision, uid, cmd+offset) != FVB_REGISTRY_OK) return 0;
        if (d->enabled && encoder.fd >= 0) {
            select_device(d);
                (void) dynamic_announce(d);
            unselect_device();
        }
        return 1;
    }
    if (!strcmp(operation, "ENABLE") || !strcmp(operation, "DISABLE")) {
        if (cmd[offset]) return 0;
        int on = !strcmp(operation, "ENABLE");
        if (fvb_registry_set_enabled(&registry, registry.revision, uid, on) != FVB_REGISTRY_OK) return 0;
        unit_generation[d-registry.devices] = 0;
        if (on && encoder.fd >= 0) {
            select_device(d);
                (void) dynamic_announce(d);
            unselect_device();
        }
        /* Disable is administrative: no native removal packet is invented. */
        return 1;
    }
    if (!strcmp(operation, "REMOVE")) {
        if (cmd[offset]) return 0;
        size_t index = (size_t)(d-registry.devices);
        if (fvb_registry_remove(&registry, registry.revision, uid) != FVB_REGISTRY_OK) return 0;
        if (index < registry.count) {
            memmove(&device_values[index], &device_values[index+1],
                (registry.count-index)*sizeof(device_values[0]));
            memmove(&unit_generation[index], &unit_generation[index+1],
                (registry.count-index)*sizeof(unit_generation[0]));
        }
        memset(&device_values[registry.count], 0, sizeof(device_values[0]));
        unit_generation[registry.count] = 0;
        return 1;
    }
    if (!d->enabled) return 0;
    if (!strcmp(operation, "UNIT") || !strcmp(operation, "ANNOUNCE")) {
        if (cmd[offset]) return 0;
        select_device(d);
        int ok = !strcmp(operation, "UNIT") ? dynamic_unit(d):dynamic_announce(d);
        unselect_device();
        return ok;
    }
    int light = d->profile == FVB_PROFILE_DIMMABLE_LIGHT || d->profile == FVB_PROFILE_COLOR_TEMPERATURE_LIGHT;
    int allowed = (!strcmp(operation, "SET") && (light || d->profile == FVB_PROFILE_SWITCH)) ||
        (!strcmp(operation, "LEVEL") && light) ||
        (!strcmp(operation, "COLOR_TEMP") && d->profile == FVB_PROFILE_COLOR_TEMPERATURE_LIGHT) ||
        (!strcmp(operation, "POSITION") && d->profile == FVB_PROFILE_COVER) ||
        (d->profile == FVB_PROFILE_THERMOSTAT && (!strcmp(operation, "TARGET") || !strcmp(operation,
        "MODE") ||
        !strcmp(operation, "TIMER") || !strcmp(operation, "SCHEDULE")));
    if (!allowed || strlen(cmd+offset) >= sizeof(rest)) return 0;
    if (d->hanfun.unit_id && unit_generation[d-registry.devices] != generation) return 0;
    strcpy(rest, cmd+offset);
    select_device(d);
    int n = snprintf(cmd, capacity, "%s %s %s", operation, endpoint_for(profile_remote(d->profile)),
        rest);
    if (n < 0 || (size_t) n >= capacity) {
        unselect_device();
        return 0;
    }
    return -1;
}

/* Caller holds lock. A full nonblocking pipe already contains a wakeup, so
 * EAGAIN is harmless; EINTR must be retried to avoid stranding the event. */
static void publish_command_event(const struct command_event *event) {
    provider_event_push(&event_queue, event);
    if (event_pipe[1] >= 0) {
        unsigned char wake = 1;
        ssize_t result;
        do {
            result = syscall(SYS_write, event_pipe[1], &wake, 1);
        }
        while (result < 0 && errno == EINTR);
    }
}

/* lock held; queue every FRITZ!-originated command for the bridge worker. */
static void queue_command_event(unsigned remote_id, unsigned has_level, unsigned has_color,
    unsigned has_cover, unsigned cover_action) {
    struct command_event event = {0};
    strcpy(event.endpoint, selected_device ? selected_device->uid:endpoint_for(remote_id));
    event.sequence = command_count;
    event.state = remote_id == 454 ? (encoder.state->cover_level < 100):
        (remote_id == 453 ? encoder.state->color_light_state:
        (remote_id == 452 ? encoder.state->hanfun_light_state:
        (remote_id == 451 ? encoder.state->light_state:encoder.state->switch_state)));
    event.remote_id = remote_id;
    event.level = remote_id == 454 ? (100-encoder.state->cover_level):
        (remote_id == 453 ? encoder.state->color_light_level:
        (remote_id == 452 ? encoder.state->hanfun_light_level:encoder.state->light_level));
    event.has_level = has_level;
    event.has_color = has_color;
    event.has_cover = has_cover;
    event.cover_action = cover_action;
    event.color_mode = encoder.state->color_mode;
    event.color_temperature = encoder.state->color_temperature;
    event.hue = encoder.state->color_hue;
    event.saturation = encoder.state->color_saturation;
    publish_command_event(&event);
}

static void queue_cover_event(unsigned action) {
    queue_command_event(454, 0, 0, 1, action);
}

static void queue_thermostat_event(void) {
    struct command_event event = {0};
    strcpy(event.endpoint, selected_device ? selected_device->uid:endpoint_for(455));
    event.sequence = command_count;
    event.remote_id = 455;
    event.has_thermostat = 1;
    event.target_temperature = encoder.state->thermostat_target;
    publish_command_event(&event);
}

static void queue_thermostat_timer_event(unsigned mode, unsigned previous, uint32_t end, uint32_t duration) {
    struct command_event event = {0};
    strcpy(event.endpoint, selected_device ? selected_device->uid:endpoint_for(455));
    event.sequence = command_count;
    event.remote_id = 455;
    event.has_thermostat_timer = 1;
    event.thermostat_timer_mode = mode;
    event.thermostat_timer_previous = previous;
    event.thermostat_timer_end = end;
    event.thermostat_timer_duration = duration;
    publish_command_event(&event);
}

int socketpair(int domain, int type, int protocol, int sv[2]) {
    if (!original_socketpair) original_socketpair = dlsym(RTLD_NEXT, "socketpair");
    int r = original_socketpair(domain, type, protocol, sv);
    if (enabled && !r && domain == AF_UNIX &&
        (type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK)) == SOCK_STREAM) {
        pthread_mutex_lock(&lock);
        if (pair_count < 32) {
            pairs[pair_count][0] = sv[0];
            pairs[pair_count++][1] = sv[1];
        }
        provider_unlock();
    }
    return r;
}

ssize_t write(int fd, const void*buf, size_t n) {
    if (!original_write) return syscall(SYS_write, fd, buf, n);
    const unsigned char*p = buf;
    if (enabled && n >= 16 && p[0] == 1 && p[1] == 3 && read_be16(p+2) == n) {
        char name[16] = {0};
        prctl(PR_GET_NAME, name, 0, 0, 0);
        if (!strncmp(name, "sR/TX", 5)) {
            pthread_mutex_lock(&lock);
            for (int i = 0; i < pair_count; i++) {
                int peer = pairs[i][0] == fd ? pairs[i][1]:(pairs[i][1] == fd ? pairs[i][0]:-1);
                if (peer >= 0) {
                    encoder.fd = fd;
                    incoming_fd = peer;
                    encoder.handle = read_be32(p+8);
                    generation++;
                    break;
                }
            }
            provider_unlock();
        }
    }
    return original_write(fd, buf, n);
}

static ssize_t send_frame(int fd, const void*buf, size_t n, int flags) {
    if (!original_send) original_send = dlsym(RTLD_NEXT, "send");
    const unsigned char*p = buf;
    if (enabled && n >= 24 && p[0] == 7 && p[1] == 3 && read_be16(p+2) == n &&
        read_be16(p+8) >= 450) {
        pthread_mutex_lock(&lock);
        unsigned remote_id = read_be16(p+8), function = read_be32(p+16);
        const fvb_device *dynamic = fvb_registry_find_remote(&registry, (uint16_t) remote_id);
        if (dynamic) {
            if (!dynamic->enabled && fd == incoming_fd) {
                provider_unlock();
                return (ssize_t) n;
            }
            if (fd != incoming_fd) {
                provider_unlock();
                return original_send(fd, buf, n, flags);
            }
            select_device(dynamic);
            remote_id = profile_remote(dynamic->profile);
            if (dynamic->hanfun.unit_id && unit_generation[dynamic-registry.devices] != generation) {
                provider_unlock();
                return original_send(fd, buf, n, flags);
            }
        }
        else if (!legacy_enabled || remote_id > 455) {
            provider_unlock();
            return original_send(fd, buf, n, flags);
        }

        DEBUG("provider: command remote=%u function=%u bytes=%zu\n", remote_id, function, n);
        if (debug_enabled && fd == incoming_fd && remote_id == 455) {
            fprintf(stderr, "provider: thermostat wire");
            for (size_t i = 16; i < n; i++) fprintf(stderr, " %02x", p[i]);
            fputc('\n', stderr);
        }
        if (fd == incoming_fd && remote_id == 455 && function == 55 && n >= 32 &&
            read_be32(p+12) == n-16 && read_be16(p+20) == n-24) {
            /* A FRITZ!App/GUI edit is never authoritative.  Consume it and
             * immediately restore the HA-owned shadow instead of forwarding
             * an ambiguous schedule mutation into Home Assistant. */
            DEBUG("provider: rejected FRITZ schedule edit, restoring HA shadow\n");
            encoder.state->thermostat_schedule_reject_until = (uint32_t) time(0)+2;
            emit_thermostat_values(&encoder);
            emit_thermostat_schedule(&encoder);
            provider_unlock();
            return n;
        }
        if (fd == incoming_fd && remote_id == 455 && function == 57 && n == 40 &&
            read_be32(p+12) == 24 && read_be16(p+20) == 16) {
            if (encoder.state->thermostat_schedule_reject_until >= (uint32_t) time(0)) {
                /* FRITZ!OS follows a timer edit with a separately generated
                 * setpoint.  It belongs to the rejected edit, not to a 440
                 * button press, so keep it away from HA as well. */
                encoder.state->thermostat_schedule_reject_until = 0;
                DEBUG("provider: rejected setpoint generated by FRITZ schedule edit\n");
                emit_thermostat_values(&encoder);
                emit_thermostat_schedule(&encoder);
                provider_unlock();
                return n;
            }
            unsigned target = p[24];
            if ((target >= 16 && target <= 56) || target == 253 || target == 254) {
                encoder.state->thermostat_target = target;
                if (target >= 16 && target <= 56) encoder.state->thermostat_heat_target = target;
                encoder.state->thermostat_activated = read_be32(p+32);
                command_count++;
                last_command = target;
                queue_thermostat_event();
                emit_thermostat_values(&encoder);
                provider_unlock();
                return n;
            }
        }
        if (fd == incoming_fd && remote_id == 455 && function == 117 && n == 40 &&
            read_be32(p+12) == 24 && read_be16(p+20) == 16) {
            unsigned mode = read_be32(p+24), previous = encoder.state->thermostat_timer_mode;
            uint32_t value = read_be32(p+28), activated = read_be32(p+32), end = 0, duration = 0;
            if (mode == 0 || mode == 2) {
                duration = value;
                end = activated+value;
                mode = mode == 0 ? 1:3;
            }
            else if (mode == 1 || mode == 3) {
                end = value;
                duration = end > activated ? end-activated:0;
            }
            else if (mode != 255) {
                provider_unlock();
                return original_send(fd, buf, n, flags);
            }
            encoder.state->thermostat_timer_mode = mode;
            encoder.state->thermostat_timer_end = mode == 255 ? 0:end;
            encoder.state->thermostat_timer_activated = activated;
            command_count++;
            last_command = mode;
            queue_thermostat_timer_event(mode, previous, encoder.state->thermostat_timer_end, duration);
            emit_thermostat_timer(&encoder);
            provider_unlock();
            return n;
        }
        if (fd == incoming_fd && (remote_id == 452 || remote_id == 453 || remote_id == 454) && function == 118 && n >= 36 &&
            read_be32(p+12) == n-16 && read_be16(p+20) == n-24 && read_be16(p+24) == 1) {
            unsigned inner_function = read_be32(p+28), inner_length = read_be16(p+32);
            if (n == 36+inner_length && remote_id != 454 && inner_function == 15 && inner_length == 4 && read_be32(p+36) <= 1) {
                unsigned value = read_be32(p+36);
                if (remote_id == 453) encoder.state->color_light_state = value;
                else encoder.state->hanfun_light_state = value;
                last_command = value;
                command_count++;
                queue_command_event(remote_id, 0, 0, 0, 0);
                if (remote_id == 453) emit_color_relay(&encoder);
                else emit_hanfun_relay(&encoder);
                provider_unlock();
                return n;
            }
            if (n == 36+inner_length && inner_function == 110 && inner_length == 8) {
                unsigned value = p[36], level_type = read_be32(p+40);
                DEBUG("provider: HANFUN level command value=%u type=%u\n", value, level_type);
                if (remote_id == 454) {
                    if (level_type == 0) encoder.state->cover_level = (value*100+127)/255;
                    else if (level_type == 1) encoder.state->cover_level = value > 100 ? 100:value;
                    else {
                        provider_unlock();
                        return original_send(fd, buf, n, flags);
                    }
                    encoder.state->cover_openclose_state = 0;
                    command_count++;
                    last_command = encoder.state->cover_level;
                    queue_cover_event(0);
                    emit_cover_status(&encoder);
                    provider_unlock();
                    return n;
                }
                unsigned *level = remote_id == 453 ? &encoder.state->color_light_level:&encoder.state->hanfun_light_level;
                if (level_type == 0)*level = (value*100+127)/255;
                else if (level_type == 1)*level = value > 100 ? 100:value;
                else if (level_type == 2)*level = (*level+(value*100+127)/255) > 100 ? 100:*level+(value*100+127)/255;
                else if (level_type == 3)*level = (*level+value) > 100 ? 100:*level+value;
                else if (level_type == 4) {
                    unsigned delta = (value*100+127)/255;
                    *level = delta > *level ? 0:*level-delta;
                }
                else if (level_type == 5)*level = value > *level ? 0:*level-value;
                command_count++;
                last_command = remote_id == 453 ? encoder.state->color_light_state:encoder.state->hanfun_light_state;
                queue_command_event(remote_id, 1, 0, 0, 0);
                if (remote_id == 453) emit_color_level(&encoder);
                else emit_hanfun_level(&encoder);
                provider_unlock();
                return n;
            }
            if (remote_id == 453 && n == 36+inner_length && inner_function == 108 && inner_length >= 4) {
                unsigned mode = read_be32(p+36);
                if (mode == 2 && inner_length == 10) {
                    encoder.state->color_mode = 2;
                    encoder.state->color_temperature = read_be16(p+40);
                    encoder.state->color_is_unmapped = 0;
                }
                else if (!dynamic && mode == 0 && inner_length == 16) {
                    encoder.state->color_mode = 0;
                    encoder.state->color_hue = read_be16(p+40);
                    encoder.state->color_saturation = p[42];
                    encoder.state->color_is_unmapped = encoder.state->color_full ? 1:(p[44] ? 1:0);
                }
                else {
                    provider_unlock();
                    return original_send(fd, buf, n, flags);
                }
                command_count++;
                last_command = encoder.state->color_light_state;
                queue_command_event(453, 0, 1, 0, 0);
                emit_color_value(&encoder);
                provider_unlock();
                return n;
            }
            if (remote_id == 454 && n == 36+inner_length && inner_function == 125 && inner_length == 8) {
                unsigned action = read_be32(p+36);
                if (action == 12 || action == 13 || action == 14) {
                    encoder.state->cover_openclose_state = 0;
                    command_count++;
                    last_command = action;
                    queue_cover_event(action);
                    emit_cover_openclose_status(&encoder);
                    provider_unlock();
                    return n;
                }
            }
        }
        if (fd == incoming_fd && (remote_id == 450 || remote_id == 451) &&
            n == 28 && read_be32(p+12) == 12 && read_be16(p+20) == 4 &&
            (function == 15 || function == 25)) {
            if (function == 15 && read_be32(p+24) <= 1) {
                unsigned value = read_be32(p+24);
                if (remote_id == 451) encoder.state->light_state = value;
                else encoder.state->switch_state = value;
                last_command = value;
                command_count++;
                queue_command_event(remote_id, 0, 0, 0, 0);
            }
            emit_relay(&encoder, remote_id, remote_id == 451 ? encoder.state->light_state:encoder.state->switch_state);
            provider_unlock();
            return n;
        }
        if (fd == incoming_fd && remote_id == 451 && function == 110 && n == 32 &&
            read_be32(p+12) == 16 && read_be16(p+20) == 8) {
            unsigned value = p[24], level_type = read_be32(p+28);
            DEBUG("provider: light level command value=%u type=%u\n", value, level_type);
            if (level_type == 0) encoder.state->light_level = (value*100+127)/255;
            else if (level_type == 1) encoder.state->light_level = value > 100 ? 100:value;
            else if (level_type == 2) encoder.state->light_level = (encoder.state->light_level+(value*100+127)/255) > 100 ? 100:encoder.state->light_level+(value*100+127)/255;
            else if (level_type == 3) encoder.state->light_level = (encoder.state->light_level+value) > 100 ? 100:encoder.state->light_level+value;
            else if (level_type == 4) {
                unsigned delta = (value*100+127)/255;
                encoder.state->light_level = delta > encoder.state->light_level ? 0:encoder.state->light_level-delta;
            }
            else if (level_type == 5) encoder.state->light_level = value > encoder.state->light_level ? 0:encoder.state->light_level-value;
            command_count++;
            last_command = encoder.state->light_state;
            queue_command_event(451, 1, 0, 0, 0);
            /* Level commands are acknowledged by the surrounding network
             * transaction. Mirroring the function payload here feeds the
             * command back into the master and creates a command loop. */
            provider_unlock();
            return n;
        }
        provider_unlock();
    }
    return original_send(fd, buf, n, flags);
}

ssize_t send(int fd, const void*buf, size_t n, int flags) {
    if (!original_send) original_send = dlsym(RTLD_NEXT, "send");
    const unsigned char *p = buf;
    /* The native stream sender can batch several complete protocol frames
     * into one send(). Validate the whole batch before consuming anything;
     * otherwise coalesced commands bypass the virtual device handler. */
    pthread_mutex_lock(&lock);
    int local = enabled && fd == incoming_fd;
    provider_unlock();
    if (!local || !provider_complete_batch(p, n))
    return original_send(fd, buf, n, flags);
    size_t offset = 0;
    if (read_be16(p+2) < n) DEBUG("provider: processing batched frames bytes=%zu\n", n);
    while (offset < n) {
        unsigned length = read_be16(p+offset+2);
        ssize_t sent = send_frame(fd, p+offset, length, flags);
        if (sent < 0) return offset ? (ssize_t) offset:sent;
        offset += (size_t) sent;
        if ((size_t) sent < length) break;
    }
    return (ssize_t) offset;
}

int close(int fd) {
    if (!original_close) return syscall(SYS_close, fd);
    if (enabled) {
        pthread_mutex_lock(&lock);
        if (fd == encoder.fd || fd == incoming_fd) {
            encoder.fd = -1;
            incoming_fd = -1;
            announced = 0;
        }
        for (int i = 0; i < pair_count; i++) if (pairs[i][0] == fd || pairs[i][1] == fd) {
            /* Retire the whole pair before fd reuse; reclaim its bounded slot
             * so repeated AHA reconnects cannot exhaust discovery forever. */
            --pair_count;
            pairs[i][0] = pairs[pair_count][0];
            pairs[i][1] = pairs[pair_count][1];
            --i;
        }
        provider_unlock();
    }
    return original_close(fd);
}

/* Caller holds lock and may have a selected device. Capture its reply before
 * restoring the legacy context; never send truncated JSON. */
static int format_control_reply(char *reply, size_t capacity, int ok,
                                const char *requested_uid) {
    int length = snprintf(reply, capacity, "{\"ok\":%s"
        ",\"generation\":%u"
        ",\"connected\":%s"
        ",\"state\":%u"
        ",\"light_state\":%u"
        ",\"light_level\":%u"
        ",\"hanfun_light_state\":%u"
        ",\"hanfun_light_level\":%u"
        ",\"color_light_state\":%u"
        ",\"color_light_level\":%u"
        ",\"color_full\":%u"
        ",\"color_mode\":%u"
        ",\"color_temperature\":%u"
        ",\"color_hue\":%u"
        ",\"color_saturation\":%u"
        ",\"color_is_unmapped\":%u"
        ",\"cover_level\":%u"
        ",\"cover_position\":%u"
        ",\"cover_openclose_state\":%u"
        ",\"thermostat_mode\":\"%s\""
        ",\"thermostat_target\":%u.%u"
        ",\"thermostat_actual\":%u.%u"
        ",\"thermostat_timer\":\"%s\""
        ",\"thermostat_timer_end\":%u"
        ",\"thermostat_schedule\":\"%s\""
        ",\"thermostat_schedule_current\":%u.%u"
        ",\"thermostat_schedule_next\":%u.%u"
        ",\"thermostat_schedule_current_minute\":%u"
        ",\"thermostat_schedule_next_minute\":%u"
        ",\"commands\":%u"
        ",\"last_command\":%u"
        ",\"server_fd\":%d}\n",
        ok ? "true":"false", generation, encoder.fd >= 0 ? "true":"false", encoder.state->switch_state,
        encoder.state->light_state, encoder.state->light_level, encoder.state->hanfun_light_state,
        encoder.state->hanfun_light_level, encoder.state->color_light_state, encoder.state->color_light_level,
        encoder.state->color_full, encoder.state->color_mode, encoder.state->color_temperature,
        encoder.state->color_hue, encoder.state->color_saturation, encoder.state->color_is_unmapped,
        encoder.state->cover_level, 100-encoder.state->cover_level, encoder.state->cover_openclose_state,
        encoder.state->thermostat_target == 253 ? "off":"heat", encoder.state->thermostat_heat_target/2,
        (encoder.state->thermostat_heat_target%2)*5, encoder.state->thermostat_actual/2, (encoder.state->thermostat_actual%2)*5,
        encoder.state->thermostat_timer_mode == 1 ? "boost":(encoder.state->thermostat_timer_mode == 3 ? "cold":"none"),
        encoder.state->thermostat_timer_end, encoder.state->thermostat_schedule_enabled ? "active":"disabled",
        encoder.state->thermostat_schedule_current/2, (encoder.state->thermostat_schedule_current%2)*5,
        encoder.state->thermostat_schedule_next/2, (encoder.state->thermostat_schedule_next%2)*5,
        encoder.state->thermostat_schedule_current_minute, encoder.state->thermostat_schedule_next_minute,
        command_count, last_command, encoder.fd);
    if (length < 0 || (size_t)length >= capacity) return -1;
    const fvb_device *reply_device = fvb_registry_find(&registry, requested_uid);
    if (reply_device) {
        size_t end = strlen(reply);
        int added = snprintf(reply+end-2, capacity-end+2,
            ""
        ",\"remote_id\":%u"
        ",\"profile\":\"%s\"}\n",
            reply_device->remote_id, fvb_profile_name(reply_device->profile));
        if (added < 0 || (size_t)added >= capacity-end+2) return -1;
        length = (int)(end-2) + added;
    }
    return length;
}

static void handle_control_client(int c, int watchers[4]) {
    struct pollfd cp = {
        c, POLLIN, 0
    };
    char cmd[256] = {0}, reply[2048];
    int keep = 0;
    if (poll(&cp, 1, 1000) > 0) {
        ssize_t n = recv(c, cmd, sizeof(cmd)-1, MSG_TRUNC);
        if (n > 0 && (size_t) n < sizeof(cmd)) {
            if (memchr(cmd, 0, (size_t) n)) {
                original_close(c);
                return;
            }
            while (n > 0 && (cmd[n-1] == '\r' || cmd[n-1] == '\n')) cmd[--n] = 0;
            if (strpbrk(cmd, "\r\n")) {
                original_close(c);
                return;
            }
            pthread_mutex_lock(&lock);
            char requested_uid[20] = {0};
                (void) sscanf(cmd, "%*s %19s", requested_uid);
            int ok = 1;
            int dynamic_result = dynamic_control(cmd, sizeof(cmd));
            if (dynamic_result >= 0) ok = dynamic_result;
            else if (!legacy_enabled && !selected_device && strcmp(cmd, "GET") && strcmp(cmd, "WATCH") && strcmp(cmd,
                "ANNOUNCE")) ok = 0;
            else if (!strcmp(cmd, "SET 0") || !strcmp(cmd, "SET 1")) {
                encoder.state->switch_state = cmd[4]-'0';
                ok = emit_relay(&encoder, 450, encoder.state->switch_state);
            }
            else if (!strncmp(cmd, "SET VIRT000000000001 ", 21) && (cmd[21] == '0' || cmd[21] == '1') && !cmd[22]) {
                encoder.state->switch_state = cmd[21]-'0';
                ok = emit_relay(&encoder, 450, encoder.state->switch_state);
            }
            else if (!strncmp(cmd, "SET VIRT000000000002 ", 21) && (cmd[21] == '0' || cmd[21] == '1') && !cmd[22]) {
                encoder.state->light_state = cmd[21]-'0';
                ok = emit_relay(&encoder, 451, encoder.state->light_state);
            }
            else if (!strncmp(cmd, "SET VIRT000000000003 ", 21) && (cmd[21] == '0' || cmd[21] == '1') && !cmd[22]) {
                encoder.state->hanfun_light_state = cmd[21]-'0';
                ok = emit_hanfun_relay(&encoder);
            }
            else if (!strncmp(cmd, "SET VIRT000000000004 ", 21) && (cmd[21] == '0' || cmd[21] == '1') && !cmd[22]) {
                encoder.state->color_light_state = cmd[21]-'0';
                ok = emit_color_relay(&encoder);
            }
            else if (!strncmp(cmd, "LEVEL VIRT000000000002 ", 23)) {
                char *end = 0;
                unsigned long value = strtoul(cmd+23, &end, 10);
                if (end == cmd+23 || *end || value > 100) ok = 0;
                else {
                    encoder.state->light_level = (unsigned) value;
                    ok = emit_light_level(&encoder);
                }
            }
            else if (!strncmp(cmd, "LEVEL VIRT000000000003 ", 23)) {
                char *end = 0;
                unsigned long value = strtoul(cmd+23, &end, 10);
                if (end == cmd+23 || *end || value > 100) ok = 0;
                else {
                    encoder.state->hanfun_light_level = (unsigned) value;
                    ok = emit_hanfun_level(&encoder);
                }
            }
            else if (!strncmp(cmd, "LEVEL VIRT000000000004 ", 23)) {
                char *end = 0;
                unsigned long value = strtoul(cmd+23, &end, 10);
                if (end == cmd+23 || *end || value > 100) ok = 0;
                else {
                    encoder.state->color_light_level = (unsigned) value;
                    ok = emit_color_level(&encoder);
                }
            }
            else if (!strncmp(cmd, "COLOR_TEMP VIRT000000000004 ", 28)) {
                char *end = 0;
                unsigned long value = strtoul(cmd+28, &end, 10);
                if (end == cmd+28 || *end || value > 65535) ok = 0;
                else {
                    encoder.state->color_mode = 2;
                    encoder.state->color_temperature = (unsigned) value;
                    encoder.state->color_is_unmapped = 0;
                    ok = emit_color_value(&encoder);
                }
            }
            else if (!strncmp(cmd, "HS VIRT000000000004 ", 20)) {
                char *end = 0;
                unsigned long hue = strtoul(cmd+20, &end, 10);
                if (end == cmd+20 || *end != ' ' || hue >= 360) ok = 0;
                else {
                    char *sat_end = 0;
                    unsigned long saturation = strtoul(end+1, &sat_end, 10);
                    if (sat_end == end+1 || *sat_end || saturation > 255) ok = 0;
                    else {
                        encoder.state->color_mode = 0;
                        encoder.state->color_hue = (unsigned) hue;
                        encoder.state->color_saturation = (unsigned) saturation;
                        encoder.state->color_is_unmapped = 1;
                        ok = emit_color_value(&encoder);
                    }
                }
            }
            else if (!strncmp(cmd, "POSITION VIRT000000000005 ", 26)) {
                char *end = 0;
                unsigned long value = strtoul(cmd+26, &end, 10);
                if (end == cmd+26 || *end || value > 100) ok = 0;
                else {
                    encoder.state->cover_level = 100-(unsigned) value;
                    encoder.state->cover_openclose_state = 0;
                    ok = emit_cover_status(&encoder);
                }
            }
            else if (!strncmp(cmd, "TARGET VIRT000000000006 ", 24)) {
                char *end = 0;
                double value = strtod(cmd+24, &end);
                double scaled = value*2.0;
                unsigned half = isfinite(value) && value >= 8.0 && value <= 28.0 ? (unsigned)(scaled+0.5):0;
                double difference = scaled-(double) half;
                if (difference < 0) difference = -difference;
                if (end == cmd+24 || *end || !isfinite(value) || value < 8.0 || value > 28.0 || difference > 0.01) ok = 0;
                else {
                    encoder.state->thermostat_target = encoder.state->thermostat_heat_target = half;
                    encoder.state->thermostat_activated = (uint32_t) time(0);
                    ok = emit_thermostat_values(&encoder);
                }
            }
            else if (!strcmp(cmd, "MODE VIRT000000000006 off")) {
                encoder.state->thermostat_target = 253;
                encoder.state->thermostat_activated = (uint32_t) time(0);
                ok = emit_thermostat_values(&encoder);
            }
            else if (!strcmp(cmd, "MODE VIRT000000000006 heat")) {
                encoder.state->thermostat_target = encoder.state->thermostat_heat_target;
                encoder.state->thermostat_activated = (uint32_t) time(0);
                ok = emit_thermostat_values(&encoder);
            }
            else if (!strncmp(cmd, "TIMER VIRT000000000006 ", 23)) {
                const char *value = cmd+23;
                if (!strcmp(value, "cancel")) {
                    encoder.state->thermostat_timer_mode = 255;
                    encoder.state->thermostat_timer_end = 0;
                    encoder.state->thermostat_timer_activated = (uint32_t) time(0);
                    ok = emit_thermostat_timer(&encoder);
                }
                else {
                    unsigned mode;
                    if (!strncmp(value, "boost ", 6)) {
                        mode = 1;
                        value += 6;
                    }
                    else if (!strncmp(value, "cold ", 5)) {
                        mode = 3;
                        value += 5;
                    }
                    else {
                        mode = 0;
                        ok = 0;
                    }
                    if (ok) {
                        uint32_t timestamp;
                        if (!fvb_parse_timestamp(value, &timestamp)) ok = 0;
                        else {
                            encoder.state->thermostat_timer_mode = mode;
                            encoder.state->thermostat_timer_end = (uint32_t) timestamp;
                            encoder.state->thermostat_timer_activated = (uint32_t) time(0);
                            ok = emit_thermostat_timer(&encoder);
                        }
                    }
                }
            }
            else if (!strcmp(cmd, "SCHEDULE VIRT000000000006 disabled")) {
                encoder.state->thermostat_schedule_enabled = 0;
                ok = emit_thermostat_schedule(&encoder);
            }
            else if (!strncmp(cmd, "SCHEDULE VIRT000000000006 active ", 33)) {
                unsigned fields[4];
                if (!fvb_parse_schedule(cmd+33, fields)) ok = 0;
                else {
                    unsigned current = fields[0], next = fields[1];
                    unsigned current_minute = fields[2], next_minute = fields[3];
                    encoder.state->thermostat_schedule_enabled = 1;
                    encoder.state->thermostat_schedule_current = current;
                    encoder.state->thermostat_schedule_next = next;
                    encoder.state->thermostat_schedule_current_minute = current_minute;
                    encoder.state->thermostat_schedule_next_minute = next_minute;
                    encoder.state->thermostat_target = encoder.state->thermostat_heat_target = encoder.state->thermostat_comfort = current;
                    encoder.state->thermostat_reduced = next;
                    encoder.state->thermostat_activated = (uint32_t) time(0);
                    ok = emit_thermostat_values(&encoder) && emit_thermostat_schedule(&encoder);
                }
            }
            else if (!strcmp(cmd, "ANNOUNCE")) {
                ok = emit_all();
            }
            else if (hanfun_experiment && !strcmp(cmd, "HANFUN UNIT")) {
                ok = emit_hanfun_unit_config(&encoder);
            }
            else if (hanfun_experiment && !strcmp(cmd, "HANFUN STATUS")) {
                ok = emit_hanfun_relay(&encoder) && emit_hanfun_level(&encoder);
            }
            else if (hanfun_experiment && !strcmp(cmd, "COLOR UNIT")) {
                ok = emit_color_unit_config(&encoder);
            }
            else if (hanfun_experiment && !strcmp(cmd, "COLOR STATUS")) {
                ok = emit_color_status(&encoder);
            }
            else if (hanfun_experiment && !strcmp(cmd, "COLOR FULL")) {
                encoder.state->color_full = 1;
                ok = emit_color_capabilities(&encoder);
            }
            else if (hanfun_experiment && !strcmp(cmd, "COLOR CT")) {
                encoder.state->color_full = 0;
                encoder.state->color_mode = 2;
                ok = emit_color_capabilities(&encoder) && emit_color_value(&encoder);
            }
            else if (hanfun_experiment && !strcmp(cmd, "BLIND UNIT")) {
                ok = emit_cover_unit_config(&encoder);
            }
            else if (hanfun_experiment && !strcmp(cmd, "BLIND STATUS")) {
                ok = emit_cover_status(&encoder);
            }
            else if (!strcmp(cmd, "WATCH")) {
                ok = 0;
                for (unsigned i = 0; i < 4; i++) if (watchers[i] < 0) {
                    watchers[i] = c;
                    keep = ok = 1;
                    break;
                }
            }
            else if (strcmp(cmd, "GET")) ok = 0;
            int reply_length = format_control_reply(reply, sizeof(reply), ok, requested_uid);
            provider_unlock();
            if (reply_length >= 0) original_send(c, reply, (size_t)reply_length, MSG_NOSIGNAL);
        }
    }
    if (!keep) original_close(c);
}

/* A bridge reconnect must not consume another permanent WATCH slot when
 * no device events arrive to expose the previous closed connection. */
static void retire_closed_watchers(int watchers[4]) {
    for (unsigned i = 0; i < 4; ++i) {
        if (watchers[i] < 0) continue;
        struct pollfd watched = {watchers[i], 0, 0};
        if (poll(&watched, 1, 0) > 0 &&
            (watched.revents & (POLLHUP | POLLERR | POLLNVAL))) {
            original_close(watchers[i]);
            watchers[i] = -1;
        }
    }
}

static void *worker(void*unused) {
    prctl(PR_SET_NAME, "virtual_provider", 0, 0, 0);
    DEBUG("provider: worker started\n");
    int listener = (int)(intptr_t) unused;
    int watchers[4] = {
        -1, -1, -1, -1
    };
    struct pollfd pf[2] = {
        {
            listener, POLLIN, 0
        }, {
            event_pipe[0], POLLIN, 0
        }
    };
    for (;;) {
        pthread_mutex_lock(&lock);
        if (encoder.fd >= 0 && announced != generation) {
            if (emit_all()) announced = generation;
        }
        provider_unlock();
        if (poll(pf, 2, 1000) <= 0) continue;
        if (pf[1].revents&POLLIN) {
            unsigned char wake[32];
            while (read(event_pipe[0], wake, sizeof(wake)) > 0) {
            }
            for (;;) {
                struct command_event ev = {0};
                int have = 0;
                pthread_mutex_lock(&lock);
                have = provider_event_pop(&event_queue, &ev);
                provider_unlock();
                if (!have) break;
                char message[224];
                int length = provider_event_format(&ev, message, sizeof(message));
                if (length < 0) {
                    DEBUG("provider: command event exceeds output buffer\n");
                    continue;
                }
                for (unsigned i = 0; i < 4; i++) if (watchers[i] >= 0 &&
                    original_send(watchers[i], message, length, MSG_DONTWAIT|MSG_NOSIGNAL) != length) {
                    original_close(watchers[i]);
                    watchers[i] = -1;
                }
            }
        }
        if (!(pf[0].revents&POLLIN)) continue;
        retire_closed_watchers(watchers);
        int c = accept(listener, 0, 0);
        if (c < 0) continue;
        handle_control_client(c, watchers);
    }
    return 0;
}
/* Create the listener before enabling hooks. A bind/listen failure must not
 * leave an active provider with no worker to deliver or receive commands. */
static int create_control_listener(void) {
    struct sockaddr_un address = {
        .sun_family = AF_UNIX
    };
    if (strlen(control_path) >= sizeof(address.sun_path)) return -1;
    strcpy(address.sun_path, control_path);
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listener < 0) return -1;
    unlink(control_path);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0) {
        original_close(listener);
        return -1;
    }
    if (chmod(control_path, 0600) < 0 || listen(listener, 4) < 0) {
        original_close(listener);
        unlink(control_path);
        return -1;
    }
    return listener;
}

static void close_event_pipe(void) {
    for (unsigned i = 0; i < 2; ++i) {
        if (event_pipe[i] >= 0) original_close(event_pipe[i]);
        event_pipe[i] = -1;
    }
}

__attribute__((constructor)) static void start(void) {
    original_write = dlsym(RTLD_NEXT, "write");
    original_send = dlsym(RTLD_NEXT, "send");
    original_close = dlsym(RTLD_NEXT, "close");
    original_socketpair = dlsym(RTLD_NEXT, "socketpair");
    const char *v = getenv("AHA_VIRTUAL_LAB");
    if (!v || strcmp(v, "1")) return;
    const char *legacy = getenv("AHA_VIRTUAL_LEGACY");
    legacy_enabled = legacy && !strcmp(legacy, "1");
    fvb_registry_init(&registry);
    debug_enabled = getenv("AHA_VIRTUAL_DEBUG") != 0;
    hanfun_experiment = getenv("AHA_VIRTUAL_HANFUN_EXPERIMENT") != 0;
    encoder.state->hanfun_timestamp = (uint32_t) time(0);
    if (!encoder.state->hanfun_timestamp) encoder.state->hanfun_timestamp = 1;
    initial_values = legacy_state;
    const char *p = getenv("AHA_VIRTUAL_CONTROL_PATH");
    if (p && *p) control_path = p;
    if (strlen(control_path) >= sizeof(((struct sockaddr_un *) 0)->sun_path)) {
        DEBUG("provider: control socket path is too long\n");
        return;
    }
    if (pipe(event_pipe)) {
        DEBUG("provider: pipe failed %d\n", errno);
        return;
    }
    for (unsigned i = 0; i < 2; ++i) {
        int flags = fcntl(event_pipe[i], F_GETFL);
        if (flags < 0 || fcntl(event_pipe[i], F_SETFL, flags | O_NONBLOCK) < 0 ||
            fcntl(event_pipe[i], F_SETFD, FD_CLOEXEC) < 0) {
            close_event_pipe();
            return;
        }
    }
    int listener = create_control_listener();
    if (listener < 0) {
        DEBUG("provider: control listener failed %d\n", errno);
        close_event_pipe();
        return;
    }
    enabled = 1;
    pthread_t t;
    int result = pthread_create(&t, 0, worker, (void *)(intptr_t) listener);
    DEBUG("provider: pthread_create=%d\n", result);
    if (!result) pthread_detach(t);
    else {
        enabled = 0;
        close_event_pipe();
        original_close(listener);
        unlink(control_path);
    }
}
