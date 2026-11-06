/*
 * picsim_runner.c -- run one HEX on picsim and dump state for validation.
 *
 * This program links against libpicsim, which is GPL-2.0; this file is
 * therefore distributed under the GNU General Public License v2.0 as well
 * (see the picsim project, https://github.com/lcgamboa/picsim).  It is a
 * standalone test helper: it is not linked into the jalv3 compiler.
 *
 * usage:
 *   picsim_runner DEVICE HEX STEPS RAMOUT [--uart PATH] [--pin RA0]
 *                 [--apin RB3:VOLTS]
 *
 * stdout (parsed by sims/picsim_validate_engine.py):
 *   pc <n>
 *   unkop <0|1>
 *   pin <NAME> <flips> <final>
 *
 * RAMOUT gets one line per RAM byte: "<hexaddr> <hexvalue>".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "picsim.h"
#include "serial.h"

static unsigned char *port_ptr(_pic *p, char c) {
    switch (c) {
    case 'A': return p->P16Emap.PORTA;
    case 'B': return p->P16Emap.PORTB;
    case 'C': return p->P16Emap.PORTC;
    case 'D': return p->P16Emap.PORTD;
    case 'E': return p->P16Emap.PORTE;
    case 'F': return p->P16Emap.PORTF;
    case 'G': return p->P16Emap.PORTG;
    }
    return NULL;
}

/* "RA0" -> 1-based picsim pin index, 0 if not found */
static int find_pin(_pic *p, const char *name) {
    unsigned char *port;
    int pord, i;
    if (!name || strlen(name) < 3) return 0;
    port = port_ptr(p, name[1]);
    if (!port) return 0;
    pord = name[2] - '0';
    for (i = 0; i < (int)p->PINCOUNT; i++)
        if (p->pins[i].port == port && p->pins[i].pord == pord) return i + 1;
    return 0;
}

int main(int argc, char **argv) {
    _pic pic;
    int proc, i, pin = 0, apin = 0, flips = 0, prev = 0;
    long k, steps;
    const char *uart = NULL, *pinname = NULL, *apinspec = NULL;
    float volts = 0.0f;
    FILE *ram;

    if (argc < 5) {
        fprintf(stderr, "usage: %s DEVICE HEX STEPS RAMOUT [--uart PATH] [--pin RA0] [--apin RB3:VOLTS]\n", argv[0]);
        return 2;
    }
    for (i = 5; i < argc; i++) {
        if (!strcmp(argv[i], "--uart") && i + 1 < argc) uart = argv[++i];
        else if (!strcmp(argv[i], "--pin") && i + 1 < argc) pinname = argv[++i];
        else if (!strcmp(argv[i], "--apin") && i + 1 < argc) apinspec = argv[++i];
    }

    memset(&pic, 0, sizeof(pic));
    proc = getprocbyname(argv[1]);
    if (!proc) { fprintf(stderr, "picsim_runner: unknown device %s\n", argv[1]); return 3; }
    if (uart) pic_set_serial(&pic, 0, uart, 0, 0, 0);
    if (pic_init(&pic, proc, argv[2], 0, 4e6)) { fprintf(stderr, "picsim_runner: pic_init failed\n"); return 4; }

    if (pinname) {
        pin = find_pin(&pic, pinname);
        if (!pin) { fprintf(stderr, "picsim_runner: pin %s not found\n", pinname); return 5; }
        prev = (*port_ptr(&pic, pinname[1])) & (1 << (pinname[2] - '0'));
    }
    if (apinspec) {
        char nm[8]; float v;
        if (sscanf(apinspec, "%7[^:]:%f", nm, &v) == 2) {
            apin = find_pin(&pic, nm);
            volts = v;
        }
        if (!apin) { fprintf(stderr, "picsim_runner: analog pin %s not found\n", apinspec); return 6; }
    }

    steps = atol(argv[3]);
    for (k = 0; k < steps; k++) {
        if (apin) {
            pic.pins[apin - 1].ptype = PT_ANALOG;
            pic.pins[apin - 1].dir = PD_IN;
            pic_set_apin(&pic, apin, volts);
        }
        pic_step(&pic);
        if (pin) {
            int v = (*port_ptr(&pic, pinname[1])) & (1 << (pinname[2] - '0'));
            if (v != prev) { flips++; prev = v; }
        }
    }

    ram = fopen(argv[4], "w");
    if (!ram) { fprintf(stderr, "picsim_runner: cannot write %s\n", argv[4]); return 7; }
    for (i = 0; i < (int)pic.RAMSIZE; i++) fprintf(ram, "%04X %02X\n", i, pic.ram[i]);
    fclose(ram);

    printf("pc %u\n", pic.pc);
    printf("unkop %d\n", (pic.error_flags & PE_UNKOP) ? 1 : 0);
    if (pin) printf("pin %s %d %d\n", pinname, flips, prev ? 1 : 0);
    return 0;
}
