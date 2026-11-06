"""Exercise the actual pad_read with a real-pad fixture and scheduled input."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
text = (root / "port/recomp_manual.c").read_text(encoding="utf-8")
start = text.index("static int pad_read(")
end = text.index("{", start) + 1
depth = 1
while depth:
    depth += (text[end] == "{") - (text[end] == "}")
    end += 1

source = r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>
typedef struct { uint16_t wButtons; uint8_t bAnalogButtons[8];
    int16_t sThumbLX,sThumbLY,sThumbRX,sThumbRY; } Pad;
typedef struct { uint32_t dwPacketNumber; Pad Gamepad; } XBOX_INPUT_STATE;
enum { XBOX_BUTTON_A, XBOX_BUTTON_B, XBOX_BUTTON_X, XBOX_BUTTON_Y,
    XBOX_BUTTON_BLACK, XBOX_BUTTON_WHITE, XBOX_BUTTON_LTRIGGER, XBOX_BUTTON_RTRIGGER };
#define XBOX_GAMEPAD_START 0x10
static struct { double t,dur; uint16_t buttons; uint8_t analog[8];
    int16_t axis[4]; int has_axis; char name[16]; int said; } s_pad_script[64];
static int s_pad_script_n=1,s_auto_n=0,s_auto_live=0;
static int16_t s_auto_lx,s_auto_ly;
static struct { double t_done; char btn[8]; int state; } s_auto[16];
static int s_talk_auto_a=0,s_cam_mode_now;
static double s_talk_auto_a_until,now;
static int host_calls,host_present=1;
static XBOX_INPUT_STATE host={42,{0x1,{0,90,0,0,0,0,31,0},1234,-2345,3456,-4567}};
static void xbox_InputInit(void) {}
static void pad_script_load(void) {}
static void auto_load(void) {}
static void trace_at_step(void) {}
static double pad_now(void) { return now; }
static int xbox_InputGetState(uint32_t port,XBOX_INPUT_STATE *out) {
    (void)port;host_calls++;if(!host_present)return 1;*out=host;return 0;
}
'''
source += text[start:end]
source += r'''
int main(void) {
    XBOX_INPUT_STATE state;
    s_pad_script[0].t=5;s_pad_script[0].dur=.25;
    s_pad_script[0].buttons=XBOX_GAMEPAD_START;
    s_pad_script[0].analog[XBOX_BUTTON_A]=255;
    s_pad_script[0].has_axis=1;s_pad_script[0].axis[2]=-765;
    for(int i=0;i<3;i++) {
        now=(double[]){1,5.1,10}[i];int calls=host_calls;
        assert(pad_read(0,&state));assert(host_calls==calls+1);
        assert(state.Gamepad.sThumbLX==1234 && state.Gamepad.sThumbLY==-2345);
        assert(state.Gamepad.sThumbRY==-4567 && state.Gamepad.bAnalogButtons[XBOX_BUTTON_B]==90);
        assert(state.Gamepad.bAnalogButtons[XBOX_BUTTON_LTRIGGER]==31);
        assert(state.Gamepad.wButtons==(i==1?0x11:0x1));
        assert(state.Gamepad.bAnalogButtons[XBOX_BUTTON_A]==(i==1?255:0));
        assert(state.Gamepad.sThumbRX==(i==1?-765:3456));
    }
    now=5.1;assert(pad_read(1,&state));assert(!memcmp(&state,&host,sizeof host));
    s_auto_live=1;s_auto_lx=-100;s_auto_ly=200;
    assert(pad_read(0,&state));assert(state.Gamepad.sThumbLX==-100 && state.Gamepad.sThumbLY==200);
    host_present=0;s_auto_live=0;assert(pad_read(0,&state));
    assert(state.Gamepad.wButtons==XBOX_GAMEPAD_START && state.Gamepad.bAnalogButtons[XBOX_BUTTON_A]==255);
    now=10;assert(pad_read(0,&state));assert(state.Gamepad.wButtons==0 && state.Gamepad.sThumbLX==0);
    puts("PASS: physical input survives before/after scripts, active presses merge, other ports and autopilot preserved");
}
'''
with tempfile.TemporaryDirectory(prefix="jsrf-pad-merge-") as folder:
    c = Path(folder) / "test.c"
    exe = Path(folder) / "test.exe"
    c.write_text(source, encoding="utf-8")
    subprocess.run(["gcc", "-std=c11", "-O2", str(c), "-lm", "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
