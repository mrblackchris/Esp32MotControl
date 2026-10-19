"""Run the actual input state machine against simulated GPIO/ADC/time."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / 'Esp32MotControl.ino').read_text()
debounce = source[source.index('struct DebouncedInput'):source.index('\nvoid updateModulation()')]
inputs = source[source.index('void serviceInputs()'):source.index('\nbool numberArg(')]
harness = r'''
#include <cassert>
#include <cmath>
#include <cstdint>
using std::fabsf;
constexpr int RUN_PIN=32, DIRECTION_PIN=33, POT_PIN=34, INPUT_PULLUP=1, LOW=0;
constexpr float LOCAL_MAX_HZ=50;
uint32_t clockMs=0;
int levels[40]={}, adc=3000;
uint32_t millis(){return clockMs;}
void pinMode(int,int){}
int digitalRead(int p){return levels[p];}
int analogRead(int){return adc;}
uint32_t analogReadMilliVolts(int p){return p==POT_PIN ? 2200 : (levels[p] ? 3100 : 0);}
uint32_t runMilliVolts=0, directionMilliVolts=0, potMilliVolts=0;
bool localMode=false, running=false, reverseDirection=false, localArmed=false;
float electricalHz=50, modulationPercent=10;
uint16_t potRaw=0;
void disconnectMotor(){running=false;}
void connectMotor(){running=true;}
void updateModulation(){}
'''.replace('using std::fabsf;','') + debounce + inputs + r'''
void tick(int ms=10){clockMs+=ms;serviceInputs();}
void change(int pin,bool active){levels[pin]=active?0:1;tick();tick(31);}
int main(){
    // Closed run at entry must not start the motor.
    levels[RUN_PIN]=0; levels[DIRECTION_PIN]=1;
    runInput.begin(); directionInput.begin();
    for(int i=0;i<60;i++)tick();
    assert(runMilliVolts==0 && directionMilliVolts==3100 && potMilliVolts==2200);
    localMode=true; tick(); assert(!running && !localArmed);
    change(RUN_PIN,false); assert(localArmed && !running);
    change(RUN_PIN,true); assert(running && !localArmed);
    // Remote stop must persist while the physical switch remains ON.
    disconnectMotor();localArmed=false;
    for(int i=0;i<20;i++){tick();}
    assert(!running);
    change(RUN_PIN,false);change(RUN_PIN,true);assert(running);
    change(DIRECTION_PIN,true);assert(!running && reverseDirection && !localArmed);
    for(int i=0;i<20;i++){tick();}
    assert(!running);
    change(RUN_PIN,false);change(RUN_PIN,true);assert(running);
    adc=0;for(int i=0;i<100;i++)tick();assert(!running && electricalHz==0);
    adc=3000;for(int i=0;i<100;i++)tick();assert(!running);
    change(RUN_PIN,false);change(RUN_PIN,true);assert(running);
    // Physical controls have no output authority in Remote mode.
    localMode=false; electricalHz=25;
    change(RUN_PIN,false);change(DIRECTION_PIN,false);
    assert(running && electricalHz==25 && reverseDirection);
}
'''
with tempfile.TemporaryDirectory() as temp:
    cpp = Path(temp) / 'inputs.cpp'
    cpp.write_text(harness)
    executable = Path(temp) / 'inputs'
    subprocess.run(['g++', '-std=c++17', '-Wall', '-Wextra', str(cpp), '-o', str(executable)], check=True)
    subprocess.run([str(executable)], check=True)
print('Local input state-machine checks passed.')
