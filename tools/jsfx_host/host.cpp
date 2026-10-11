// A C interface over ysfx, so Python tests can run a JSFX without REAPER.
// ysfx is an independent JSFX host. It is not REAPER, so a pass here does not replace a
// REAPER run. It does run the real EEL2 code and catches logic and syntax errors.
#include "ysfx.h"
#include "ysfx.hpp"
#include "WDL/eel2/ns-eel.h"
#include <cstdio>
#include <cstring>
#include <string>

static std::string last_log;

static void reporter(intptr_t, ysfx_log_level level, const char *message)
{
    last_log += ysfx_log_level_string(level);
    last_log += ": ";
    last_log += message;
    last_log += "\n";
}

extern "C" {

const char *fh_log() { return last_log.c_str(); }

void *fh_new(const char *path, double srate, int block)
{
    ysfx_config_t *config = ysfx_config_new();
    ysfx_set_log_reporter(config, &reporter);
    ysfx_guess_file_roots(config, path);
    ysfx_t *fx = ysfx_new(config);
    ysfx_config_free(config);
    if (!ysfx_load_file(fx, path, 0) || !ysfx_compile(fx, 0)) {
        ysfx_free(fx);
        return nullptr;
    }
    ysfx_set_sample_rate(fx, srate);
    ysfx_set_block_size(fx, (uint32_t)block);
    ysfx_init(fx);
    return fx;
}

void fh_free(void *fx) { ysfx_free((ysfx_t *)fx); }

// Runs @init again, as REAPER does when an FX comes back online.
void fh_init(void *fx) { ysfx_init((ysfx_t *)fx); }

void fh_time(void *fx, double tempo, int state, double beat, double seconds)
{
    ysfx_time_info_t info{};
    info.tempo = tempo;
    info.playback_state = (uint32_t)state;
    info.beat_position = beat;
    info.time_position = seconds;
    info.time_signature[0] = 4;
    info.time_signature[1] = 4;
    ysfx_set_time_info((ysfx_t *)fx, &info);
}

// Runs one block and writes each MIDI event as five ints: bus, offset, status, data1, data2.
int fh_process(void *fx, int frames, int *events, int capacity)
{
    ysfx_t *f = (ysfx_t *)fx;
    double left[4096] = {0}, right[4096] = {0};
    double *ins[2] = {left, right};
    double *outs[2] = {left, right};
    ysfx_process_double(f, ins, outs, 0, 0, (uint32_t)frames);
    int count = 0;
    ysfx_midi_event_t event;
    while (ysfx_receive_midi(f, &event)) {
        if (count < capacity) {
            int *row = events + count * 5;
            row[0] = (int)event.bus;
            row[1] = (int)event.offset;
            row[2] = event.size > 0 ? event.data[0] : 0;
            row[3] = event.size > 1 ? event.data[1] : 0;
            row[4] = event.size > 2 ? event.data[2] : 0;
        }
        ++count;
    }
    return count;
}

double fh_slider(void *fx, int index) { return ysfx_slider_get_value((ysfx_t *)fx, (uint32_t)index); }

void fh_set_slider(void *fx, int index, double value)
{
    ysfx_slider_set_value((ysfx_t *)fx, (uint32_t)index, value, true);
}

double fh_var(void *fx, const char *name) { return ysfx_read_var((ysfx_t *)fx, name); }

void fh_read_mem(void *fx, unsigned addr, double *out, int count)
{
    ysfx_read_vmem((ysfx_t *)fx, addr, out, (uint32_t)count);
}

void fh_write_mem(void *fx, unsigned addr, const double *in, int count)
{
    ysfx_t *f = (ysfx_t *)fx;
    for (int i = 0; i < count; ++i) {
        int valid = 0;
        EEL_F *cell = NSEEL_VM_getramptr(f->vm.get(), addr + (unsigned)i, &valid);
        if (cell)
            *cell = in[i];
    }
}

// Copies the @serialize data out. Returns its size, or the size needed when capacity is short.
int fh_save(void *fx, unsigned char *out, int capacity)
{
    ysfx_state_t *state = ysfx_save_state((ysfx_t *)fx);
    if (!state)
        return -1;
    int size = (int)state->data_size;
    if (size <= capacity)
        memcpy(out, state->data, state->data_size);
    ysfx_state_free(state);
    return size;
}

// Loads only @serialize data, as REAPER does on undo.
int fh_load(void *fx, const unsigned char *data, int size)
{
    ysfx_state_t state{};
    state.data = (uint8_t *)data;
    state.data_size = (size_t)size;
    return ysfx_load_serialized_state((ysfx_t *)fx, &state) ? 1 : 0;
}

}
