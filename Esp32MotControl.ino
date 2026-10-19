#include <WiFi.h>
#include <WebServer.h>
#include <math.h>
#include <errno.h>
#include "web_page.h"
#include "driver/mcpwm_prelude.h"
#include "driver/gpio.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_struct.h"
#include "soc/gpio_sig_map.h"
#include "soc/soc_caps.h"
#include "esp_system.h"

#if !CONFIG_IDF_TARGET_ESP32
#error "This pin map and two MCPWM groups require a classic ESP32."
#endif

#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#include "wifi_config.example.h"
#endif
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;


WebServer server(80);
bool serialRequest = false;
uint32_t serialRequestId = 0;
String serialKeys[20], serialValues[20];
int serialArgumentCount = 0;

String jsonString(const String& value) {
    String result = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned int i = 0; i < value.length(); ++i) {
        uint8_t c = value[i];
        if (c == '"' || c == '\\') { result += '\\'; result += char(c); }
        else if (c < 32) { result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
        else result += char(c);
    }
    return result + '"';
}

bool requestHasArg(const char* name) {
    if (!serialRequest) return server.hasArg(name);
    for (int i = 0; i < serialArgumentCount; ++i) if (serialKeys[i] == name) return true;
    return false;
}
String requestArg(const char* name) {
    if (!serialRequest) return server.arg(name);
    for (int i = 0; i < serialArgumentCount; ++i) if (serialKeys[i] == name) return serialValues[i];
    return "";
}
void respond(int code, const char* type, const String& body) {
    if (!serialRequest) {
        server.sendHeader("Cache-Control", "no-store");
        server.sendHeader("Access-Control-Allow-Origin", "*");
        server.send(code, type, body); return;
    }
    Serial.print("MC {\"id\":"); Serial.print(serialRequestId);
    Serial.print(",\"code\":"); Serial.print(code);
    Serial.print(",\"data\":"); Serial.print(String(type) == "application/json" ? body : jsonString(body));
    Serial.println('}');
}


// U retains the original half bridge. Confirm these pins against your board.
const int HIGH_PINS[3] = {18, 21, 23};
const int LOW_PINS[3] = {19, 22, 26};
constexpr int FG_PIN = 25;
constexpr uint32_t PWM_CLOCK = 10000000;
uint32_t carrierHz = 20000;
uint32_t periodTicks = PWM_CLOCK / 20000;
volatile uint32_t halfPeriod = PWM_CLOCK / (2 * 20000);
volatile bool modulationUpdates = true;

float actualCarrierHz() { return float(PWM_CLOCK) / periodTicks; }
constexpr unsigned TABLE_SIZE = 1024;
int16_t spaceVector[TABLE_SIZE][3];
mcpwm_timer_handle_t motorTimer, pulseTimer;
mcpwm_oper_handle_t motorOperators[3];
mcpwm_cmpr_handle_t motorComparators[3], pulseComparator;
mcpwm_gen_handle_t highGenerators[3], lowGenerators[3];
uint32_t motorSignals[6];
portMUX_TYPE controlLock = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t phaseAccumulator = 0, phaseStep = 0;
volatile uint32_t amplitude = 0;
float electricalHz = 50, modulationPercent = 10, deadUs = 1;
uint32_t tonUs = 20, toffUs = 80;
bool running = false;
constexpr int RUN_PIN = 32, DIRECTION_PIN = 33, POT_PIN = 34;
constexpr float LOCAL_MAX_HZ = 50.0f;
bool localMode = false, reverseDirection = false, localArmed = false;
uint16_t potRaw = 0;
struct DebouncedInput {
    int pin;
    bool raw = false, active = false;
    uint32_t changedAt = 0;
    void begin() {
        pinMode(pin, INPUT_PULLUP);
        raw = active = digitalRead(pin) == LOW;
        changedAt = millis();
    }
    bool update() {
        bool next = digitalRead(pin) == LOW;
        if (next != raw) { raw = next; changedAt = millis(); }
        if (active != raw && uint32_t(millis() - changedAt) >= 30) {
            active = raw; return true;
        }
        return false;
    }
};
DebouncedInput runInput{RUN_PIN}, directionInput{DIRECTION_PIN};

void updateModulation() {
    uint32_t deadTicks = lroundf(deadUs * PWM_CLOCK / 1000000.0f);
    uint32_t maxAmplitude = halfPeriod / 2 - deadTicks - 2;
    uint32_t step = uint32_t(double(electricalHz) * 4294967296.0 / actualCarrierHz());
    portENTER_CRITICAL(&controlLock);
    phaseStep = reverseDirection ? uint32_t(0) - step : step;
    amplitude = lroundf(maxAmplitude * modulationPercent / 100.0f);
    portEXIT_CRITICAL(&controlLock);
}


// Min/max common-mode injection is equivalent to centered, seven-segment
// SVPWM in the linear range. Phase references are 0, 120 and 240 degrees.
void buildTable() {
    for (unsigned i = 0; i < TABLE_SIZE; ++i) {
        float v[3];
        for (int k = 0; k < 3; ++k)
            v[k] = sinf(2 * PI * i / TABLE_SIZE + k * 2 * PI / 3);
        float common = -0.5f * (fmaxf(v[0], fmaxf(v[1], v[2])) +
                                fminf(v[0], fminf(v[1], v[2])));
        for (int k = 0; k < 3; ++k)
            spaceVector[i][k] = lroundf((v[k] + common) / sqrtf(3.0f) * 32767);
    }
}

// Prepare all three shadow compares at the carrier peak. They latch together
// at the following zero, leaving half a carrier period for the ISR to finish.
bool IRAM_ATTR onCarrierPeak(mcpwm_timer_handle_t,
                            const mcpwm_timer_event_data_t*, void*) {
    portENTER_CRITICAL_ISR(&controlLock);
    if (!modulationUpdates) {
        portEXIT_CRITICAL_ISR(&controlLock);
        return false;
    }
    phaseAccumulator += phaseStep;
    unsigned index = phaseAccumulator >> 22;
    for (int k = 0; k < 3; ++k) {
        int32_t offset = (int32_t(spaceVector[index][k]) * int32_t(amplitude)) / 32767;
        mcpwm_comparator_set_compare_value(motorComparators[k], halfPeriod / 2 + offset);
    }
    portEXIT_CRITICAL_ISR(&controlLock);
    return false;
}

// Disconnect the PWM matrix so BOTH gate inputs stay low, including inverted
// complementary outputs. A stopped timer alone would retain its last levels.
void disconnectMotor() {
    for (int k = 0; k < 3; ++k) {
        for (int pin : {HIGH_PINS[k], LOW_PINS[k]}) {
            gpio_set_level((gpio_num_t)pin, 0);
            esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
            gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
        }
    }
    running = false;
}

void connectMotor() {
    for (int k = 0; k < 3; ++k) {
        esp_rom_gpio_connect_out_signal(HIGH_PINS[k], motorSignals[2*k], false, false);
        esp_rom_gpio_connect_out_signal(LOW_PINS[k], motorSignals[2*k+1], false, false);
    }
    running = true;
}

void configureMotor() {
    disconnectMotor();
    uint32_t deadTicks = lroundf(deadUs * PWM_CLOCK / 1000000.0f);
    for (int k = 0; k < 3; ++k) {
        mcpwm_dead_time_config_t dt = {};
        dt.posedge_delay_ticks = deadTicks;
        ESP_ERROR_CHECK(mcpwm_generator_set_dead_time(highGenerators[k], highGenerators[k], &dt));
        dt = {};
        dt.negedge_delay_ticks = deadTicks;
        dt.flags.invert_output = true;
        ESP_ERROR_CHECK(mcpwm_generator_set_dead_time(highGenerators[k], lowGenerators[k], &dt));
    }
    updateModulation();
}

void setupMotor() {
    buildTable();
    mcpwm_timer_config_t tc = {};
    tc.group_id = 0;
    tc.clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT;
    tc.resolution_hz = PWM_CLOCK;
    tc.count_mode = MCPWM_TIMER_COUNT_MODE_UP_DOWN;
    // The driver takes the FULL up/down period and divides it by two
    // internally to obtain the peak used for comparator range checks.
    tc.period_ticks = periodTicks;
    ESP_ERROR_CHECK(mcpwm_new_timer(&tc, &motorTimer));
    for (int k = 0; k < 3; ++k) {
        mcpwm_operator_config_t oc = {};
        oc.group_id = 0;
        ESP_ERROR_CHECK(mcpwm_new_operator(&oc, &motorOperators[k]));
        ESP_ERROR_CHECK(mcpwm_operator_connect_timer(motorOperators[k], motorTimer));
        mcpwm_comparator_config_t cc = {};
        cc.flags.update_cmp_on_tez = true;
        ESP_ERROR_CHECK(mcpwm_new_comparator(motorOperators[k], &cc, &motorComparators[k]));
        ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(motorComparators[k], halfPeriod / 2));
        mcpwm_generator_config_t gc = {};
        gc.gen_gpio_num = HIGH_PINS[k];
        ESP_ERROR_CHECK(mcpwm_new_generator(motorOperators[k], &gc, &highGenerators[k]));
        gc.gen_gpio_num = LOW_PINS[k];
        ESP_ERROR_CHECK(mcpwm_new_generator(motorOperators[k], &gc, &lowGenerators[k]));
        ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(highGenerators[k],
            MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
        ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(highGenerators[k],
            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, motorComparators[k], MCPWM_GEN_ACTION_LOW)));
        ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(highGenerators[k],
            MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_DOWN, motorComparators[k], MCPWM_GEN_ACTION_HIGH)));
        motorSignals[2*k] = GPIO.func_out_sel_cfg[HIGH_PINS[k]].func_sel;
        motorSignals[2*k+1] = GPIO.func_out_sel_cfg[LOW_PINS[k]].func_sel;
    }
    configureMotor();
    mcpwm_timer_event_callbacks_t callbacks = {};
    callbacks.on_full = onCarrierPeak;
    ESP_ERROR_CHECK(mcpwm_timer_register_event_callbacks(motorTimer, &callbacks, nullptr));
    ESP_ERROR_CHECK(mcpwm_timer_enable(motorTimer));
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(motorTimer, MCPWM_TIMER_START_NO_STOP));
}

void setupPulse() {
    mcpwm_timer_config_t tc = {};
    tc.group_id = 1;
    tc.clk_src = MCPWM_TIMER_CLK_SRC_DEFAULT;
    tc.resolution_hz = 1000000;
    tc.count_mode = MCPWM_TIMER_COUNT_MODE_UP;
    tc.period_ticks = tonUs + toffUs;
    tc.flags.update_period_on_empty = true;
    ESP_ERROR_CHECK(mcpwm_new_timer(&tc, &pulseTimer));
    mcpwm_oper_handle_t op;
    mcpwm_operator_config_t oc = {};
    oc.group_id = 1;
    ESP_ERROR_CHECK(mcpwm_new_operator(&oc, &op));
    ESP_ERROR_CHECK(mcpwm_operator_connect_timer(op, pulseTimer));
    mcpwm_comparator_config_t cc = {};
    cc.flags.update_cmp_on_tez = true;
    ESP_ERROR_CHECK(mcpwm_new_comparator(op, &cc, &pulseComparator));
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(pulseComparator, tonUs));
    mcpwm_gen_handle_t gen;
    mcpwm_generator_config_t gc = {};
    gc.gen_gpio_num = FG_PIN;
    ESP_ERROR_CHECK(mcpwm_new_generator(op, &gc, &gen));
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_timer_event(gen,
        MCPWM_GEN_TIMER_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, MCPWM_TIMER_EVENT_EMPTY, MCPWM_GEN_ACTION_HIGH)));
    ESP_ERROR_CHECK(mcpwm_generator_set_action_on_compare_event(gen,
        MCPWM_GEN_COMPARE_EVENT_ACTION(MCPWM_TIMER_DIRECTION_UP, pulseComparator, MCPWM_GEN_ACTION_LOW)));
    ESP_ERROR_CHECK(mcpwm_timer_enable(pulseTimer));
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(pulseTimer, MCPWM_TIMER_START_NO_STOP));
}

String stateJson() {
    String out = "{\"running\":" + String(running ? "true" : "false");
    out += ",\"mode\":" + jsonString(localMode ? "local" : "remote");
    out += ",\"reverse\":" + String(reverseDirection ? "true" : "false");
    out += ",\"freq\":" + String(electricalHz, 3);
    out += ",\"mod\":" + String(modulationPercent, 3);
    out += ",\"carrier\":" + String(carrierHz);
    out += ",\"actualCarrier\":" + String(actualCarrierHz(), 3);
    out += ",\"dead\":" + String(deadUs, 3);
    out += ",\"ton\":" + String(tonUs) + ",\"toff\":" + String(toffUs);
    out += ",\"runSwitch\":" + String(runInput.active ? "true" : "false");
    out += ",\"directionSwitch\":" + String(directionInput.active ? "true" : "false");
    out += ",\"localArmed\":" + String(localArmed ? "true" : "false");
    out += ",\"potRaw\":" + String(potRaw);
    out += ",\"localMaxHz\":" + String(LOCAL_MAX_HZ, 1);
    out += ",\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
    out += ",\"ip\":" + jsonString(WiFi.localIP().toString()) + "}";
    return out;
}

void handleStop() {
    disconnectMotor();
    localArmed = false; // A held ON switch cannot undo a remote stop.
    respond(200, "application/json", stateJson());
}

void handleMode() {
    String mode = requestArg("mode");
    if (mode != "local" && mode != "remote") {
        respond(400, "text/plain", "mode must be local or remote."); return;
    }
    disconnectMotor();
    localMode = mode == "local";
    localArmed = false;
    respond(200, "application/json", stateJson());
}

void serviceInputs() {
    bool runChanged = runInput.update();
    bool directionChanged = directionInput.update();
    static uint32_t lastSample = 0;
    static float filtered = 0;
    if (uint32_t(millis() - lastSample) >= 10) {
        lastSample = millis();
        filtered += (analogRead(POT_PIN) - filtered) * 0.125f;
        potRaw = lroundf(filtered);
    }
    if (!localMode) return;
    if (!runInput.active) {
        disconnectMotor();
        localArmed = true;
    }
    if (directionChanged && running) {
        disconnectMotor();
        localArmed = false; // Require OFF then ON before reversing.
    }
    if (!running && reverseDirection != directionInput.active) {
        reverseDirection = directionInput.active;
        updateModulation();
    }
    float target = potRaw < 40 ? 0 : LOCAL_MAX_HZ * potRaw / 4095.0f;
    if (fabsf(target - electricalHz) >= 0.1f || (target == 0 && electricalHz != 0)) {
        electricalHz = target;
        updateModulation();
    }
    if (electricalHz == 0 || modulationPercent == 0) disconnectMotor();
    // Only an actual OFF->ON edge starts the motor; raising the pot cannot restart it.
    if (runChanged && runInput.active && localArmed) {
        localArmed = false;
        if (electricalHz > 0 && modulationPercent > 0) connectMotor();
    }
}

bool numberArg(const char* name, double minimum, double maximum, double& value) {
    if (!requestHasArg(name)) return false;
    String input = requestArg(name);
    char* end;
    errno = 0;
    value = strtod(input.c_str(), &end);
    return end != input.c_str() && *end == '\0' && errno == 0 &&
           isfinite(value) && value >= minimum && value <= maximum;
}

void handleSet() {
    double hz, carrier, mod, dead, ton, toff;
    if (!numberArg("carrier", 5000, 20000, carrier) || carrier != floor(carrier) ||
        !numberArg("freq", 0, 1500, hz) || !numberArg("mod", 0, 100, mod) ||
        !numberArg("dead", 0.1, 5, dead) || !numberArg("ton", 1, 30000, ton) ||
        !numberArg("toff", 1, 30000, toff) || ton != floor(ton) || toff != floor(toff) ||
        !requestHasArg("run") || (requestArg("run") != "0" && requestArg("run") != "1")) {
        respond(400, "text/plain", "Invalid settings. Check the displayed limits.");
        return;
    }
    if (localMode) {
        respond(409, "text/plain", "Select Remote mode before applying remote settings."); return;
    }
    bool nextReverse = reverseDirection;
    if (requestHasArg("reverse")) {
        if (requestArg("reverse") != "0" && requestArg("reverse") != "1") {
            respond(400, "text/plain", "reverse must be 0 or 1."); return;
        }
        nextReverse = requestArg("reverse") == "1";
    }
    if (running && nextReverse != reverseDirection) {
        respond(409, "text/plain", "Stop before changing direction."); return;
    }
    reverseDirection = nextReverse;
    disconnectMotor();
    // Block ISR writes before shrinking the timer range. Stop at zero,
    // then change the full period and all comparator values together.
    portENTER_CRITICAL(&controlLock);
    modulationUpdates = false;
    portEXIT_CRITICAL(&controlLock);
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(motorTimer, MCPWM_TIMER_STOP_EMPTY));
    delay(2); // at least ten cycles at the minimum 5 kHz carrier
    carrierHz = uint32_t(carrier);
    periodTicks = 2 * uint32_t(lround(double(PWM_CLOCK) / (2 * carrierHz)));
    halfPeriod = periodTicks / 2;
    ESP_ERROR_CHECK(mcpwm_timer_set_period(motorTimer, periodTicks));
    for (int k = 0; k < 3; ++k)
        ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(motorComparators[k], halfPeriod / 2));
    electricalHz = hz; modulationPercent = mod; deadUs = dead;
    tonUs = ton; toffUs = toff;
    configureMotor();
    portENTER_CRITICAL(&controlLock);
    modulationUpdates = true;
    portEXIT_CRITICAL(&controlLock);
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(motorTimer, MCPWM_TIMER_START_NO_STOP));
    // Stop at zero before changing both pulse timing values together.
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(pulseTimer, MCPWM_TIMER_STOP_EMPTY));
    delay(65); // maximum supported pulse period is 60 ms
    ESP_ERROR_CHECK(mcpwm_timer_set_period(pulseTimer, tonUs + toffUs));
    ESP_ERROR_CHECK(mcpwm_comparator_set_compare_value(pulseComparator, tonUs));
    ESP_ERROR_CHECK(mcpwm_timer_start_stop(pulseTimer, MCPWM_TIMER_START_NO_STOP));
    // Allow the motor compares and dead-time configuration to settle.
    delay(1);
    if (requestArg("run") == "1" && electricalHz > 0 && modulationPercent > 0) connectMotor();
    respond(200, "application/json", stateJson());
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
bool decodeArgument(const String& input, String& output) {
    output = "";
    for (unsigned int i = 0; i < input.length(); ++i) {
        char c = input[i];
        if (c == '%') {
            if (i + 2 >= input.length()) return false;
            int high = hexDigit(input[i + 1]), low = hexDigit(input[i + 2]);
            if (high < 0 || low < 0) return false;
            c = char(high * 16 + low); i += 2;
        } else if (c == '+') c = ' ';
        if ((uint8_t)c < 32 || c == 127) return false;
        output += c;
    }
    return true;
}
void dispatchSerial(const String& line) {
    if (!line.startsWith("MC ")) return;
    serialRequest = true; serialRequestId = 0; serialArgumentCount = 0;
    int space = line.indexOf(' ', 3);
    String id = space < 0 ? "" : line.substring(3, space);
    bool valid = id.length() > 0 && id.length() <= 9;
    for (unsigned int i = 0; i < id.length(); ++i) if (!isDigit(id[i])) valid = false;
    if (valid) serialRequestId = strtoul(id.c_str(), nullptr, 10);
    if (!valid || serialRequestId == 0) {
        respond(400, "text/plain", "Invalid request ID."); serialRequest = false; return;
    }
    String path = line.substring(space + 1);
    int query = path.indexOf('?');
    if (query >= 0) {
        String args = path.substring(query + 1); path = path.substring(0, query);
        int start = 0;
        while (start < int(args.length())) {
            int end = args.indexOf('&', start); if (end < 0) end = args.length();
            String pair = args.substring(start, end);
            int equals = pair.indexOf('=');
            if (serialArgumentCount >= 20 || equals <= 0 ||
                !decodeArgument(pair.substring(0, equals), serialKeys[serialArgumentCount]) ||
                !decodeArgument(pair.substring(equals + 1), serialValues[serialArgumentCount])) { valid = false; break; }
            for (int i = 0; i < serialArgumentCount; ++i)
                if (serialKeys[i] == serialKeys[serialArgumentCount]) valid = false;
            if (!valid) break;
            ++serialArgumentCount; start = end + 1;
        }
    }
    if (!valid) respond(400, "text/plain", "Malformed or duplicate URL-encoded arguments.");
    else if (path == "/state") respond(200, "application/json", stateJson());
    else if (path == "/set") handleSet();
    else if (path == "/stop") handleStop();
    else if (path == "/mode") handleMode();
    else respond(404, "text/plain", "Unknown serial command.");
    // Do not retain credentials in the parser after the response.
    for (int i = 0; i < 20; ++i) { serialKeys[i] = ""; serialValues[i] = ""; }
    serialRequest = false;
}
void serviceSerial() {
    static String line;
    static bool overflow = false;
    // Bound work per loop so incoming traffic cannot starve burst servicing.
    for (int budget = 0; budget < 256 && Serial.available(); ++budget) {
        char c = Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            if (!overflow) dispatchSerial(line);
            else Serial.println("MC {\"id\":0,\"code\":400,\"data\":\"Command exceeds 1536 bytes.\"}");
            line = ""; overflow = false;
        } else if (!overflow) {
            if (line.length() >= 1536) { line = ""; overflow = true; }
            else line += c;
        }
    }
}

void setup() {
    Serial.setRxBufferSize(2048);
    Serial.begin(115200);
    Serial.printf("Motor controller boot; reset reason=%d (esp_reset_reason)\n", int(esp_reset_reason()));
    // Hold every motor input low before allocating peripherals.
    disconnectMotor();
    setupMotor();
    setupPulse();
    runInput.begin(); directionInput.begin();
    analogReadResolution(12);
    pinMode(POT_PIN, INPUT);
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    server.on("/", HTTP_GET, [] { server.send_P(200, "text/html", PAGE); });
    server.on("/set", HTTP_POST, handleSet);
    server.on("/stop", HTTP_POST, handleStop);
    server.on("/mode", HTTP_POST, handleMode);
    server.on("/state", HTTP_GET, [] { respond(200, "application/json", stateJson()); });
    server.begin();
}

void loop() {
    serviceInputs();
    serviceSerial();
    server.handleClient();
    serviceInputs();
    static bool reported = false;
    if (!reported && WiFi.status() == WL_CONNECTED) {
        Serial.print("Web interface: http://"); Serial.println(WiFi.localIP());
        reported = true;
    }
    delay(1);
}
