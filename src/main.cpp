#include <Arduino.h>
#include <WiFi.h>
#include <FastAccelStepper.h>
#include <TMCStepper.h>
#include <WiFiManager.h>
#include <ArduinoOTA.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <WiFiUdp.h>
#include <OSCMessage.h>
#include <OSCBundle.h>
#include <OSCData.h>
#include <math.h>

//==================== Configuration ====================
#define NUM_MOTORS 4

//==================== NEW: Presets, offsets, mapping ====================
struct Preset { long p, t, z, s; };
Preset presets[8];         // utilitaires: /preset/set i p t z s
int activePreset = -1;

// Offsets joystick (en steps)
volatile long pan_offset_steps  = 0;
volatile long tilt_offset_steps = 0;
long PAN_OFFSET_RANGE  = 800;   // max offset +/- (configurable via OSC)
long TILT_OFFSET_RANGE = 800;

// Mapping linéaire slide -> compensation pan/tilt (en steps)
long PAN_AT_SLIDE_MIN  = +800;  // ex: +800 à gauche
long PAN_AT_SLIDE_MAX  = -800;  // ex: -800 à droite
long TILT_AT_SLIDE_MIN = 0;
long TILT_AT_SLIDE_MAX = 0;

// Mouvement synchronisé en cours
struct SyncMove {
  bool  active = false;
  uint32_t t0_ms = 0;
  uint32_t T_ms  = 2000;      // durée demandée
  long start[NUM_MOTORS];
  long goal_base[NUM_MOTORS]; // cible sans offsets/couplages
} sync_move;

// Jog slide
float slide_jog_cmd = 0.0f;    // -1..+1
float SLIDE_JOG_SPEED = 6000;  // steps/s @ |cmd|=1 (à ajuster)

// Jog Pan/Tilt (vitesses de jog)
static const float PAN_JOG_SPEED  = 3000.0f; // steps/s @ |joy|=1
static const float TILT_JOG_SPEED = 3000.0f;

// Pins STEP/DIR/EN
const int STEP_PINS[NUM_MOTORS]    = {18, 21, 23, 26};
const int DIR_PINS[NUM_MOTORS]     = {19, 22, 25, 27};
const int ENABLE_PINS[NUM_MOTORS]  = {13, 14, 32, 33};

// UART TMC2209
#define UART_TX 17
#define UART_RX 16
#define ADDR_PAN   0b00
#define ADDR_TILT  0b01
#define ADDR_ZOOM  0b10
#define ADDR_SLIDE 0b11
#define R_SENSE 0.11f

// Configuration des axes
struct AxisConfig {
  long min_limit;
  long max_limit;
  int  current_ma;
  int  microsteps;
  int  max_speed;
  int  max_accel;
  int  sgt;
  bool coolstep;
  bool spreadcycle;
  bool stallguard;
};

AxisConfig cfg[NUM_MOTORS] = {
  // Pan
  {-10000, 10000, 800, 16, 20000, 8000, 0, false, true, false},
  // Tilt  
  {-10000, 10000, 800, 16, 20000, 8000, 0, false, true, false},
  // Zoom
  {-20000, 20000, 800, 16, 20000, 8000, 0, false, true, false},
  // Slide
  {-20000, 20000, 800, 16, 20000, 8000, 0, false, true, false}
};

//==================== Objets moteurs ====================
FastAccelStepperEngine engine;
FastAccelStepper* steppers[NUM_MOTORS];

TMC2209Stepper driver_pan  (&Serial2, R_SENSE, ADDR_PAN);
TMC2209Stepper driver_tilt (&Serial2, R_SENSE, ADDR_TILT);
TMC2209Stepper driver_zoom (&Serial2, R_SENSE, ADDR_ZOOM);
TMC2209Stepper driver_slide(&Serial2, R_SENSE, ADDR_SLIDE);
TMC2209Stepper* drivers[NUM_MOTORS] = {&driver_pan,&driver_tilt,&driver_zoom,&driver_slide};

//==================== Setup Drivers TMC ====================
void setupDriversTMC() {
  Serial2.begin(115200, SERIAL_8N1, UART_RX, UART_TX);
  delay(50);

  for (int i=0; i<NUM_MOTORS; i++) {
    auto d = drivers[i];
    d->begin();
    d->toff(5);                                // enable driver
    d->rms_current(cfg[i].current_ma);         // courant RMS
    d->microsteps(cfg[i].microsteps);          // µsteps
    d->pwm_autoscale(true);                    // pour StealthChop
    d->en_spreadCycle(cfg[i].spreadcycle);
    d->SGTHRS(cfg[i].sgt);                     // StallGuard threshold
    // d->coolstep_en(cfg[i].coolstep);  // Pas disponible sur TMC2209
    // d->stallguard(cfg[i].stallguard);  // Pas disponible sur TMC2209
  }
}

//==================== NEW: Helpers ====================
static inline long clampL(long v, long vmin, long vmax){ return v < vmin ? vmin : (v > vmax ? vmax : v); }
static inline float clampF(float v, float vmin, float vmax){ return v < vmin ? vmin : (v > vmax ? vmax : v); }
static inline float lerp(float a, float b, float u){ return a + (b - a) * u; }

// Minimum-jerk s(t) = 10τ^3 - 15τ^4 + 6τ^5 avec τ = t/T
// ds/dt max = 1.875/T ; d2s/dt2 max ≈ 5.7735/T^2
static inline float s_minjerk(float tau){
  tau = clampF(tau, 0.0f, 1.0f);
  return 10*tau*tau*tau - 15*tau*tau*tau*tau + 6*tau*tau*tau*tau*tau;
}

//==================== NEW: Joystick Pipeline ====================
struct JoyCfg { 
  float deadzone=0.06f, expo=0.35f, slew_per_s=8000.0f, filt_hz=60.0f; 
} joy;

struct JoyState { 
  float pan=0, tilt=0, slide=0; 
};

volatile JoyState joy_raw;  // alimenté par l'OSC
static   JoyState joy_cmd, joy_filt;

static inline float apply_deadzone_expo(float x, float dz, float expo){
  x = clampF(x, -1.f, 1.f); 
  if (fabsf(x) <= dz) return 0.f;
  float s = x >= 0 ? 1.f : -1.f, u = (fabsf(x) - dz) / (1.f - dz);
  return s * ((1-expo) * u + expo * u * u * u);
}

static inline float iir_1pole(float y, float x, float f, float dt){ 
  if(f <= 0) return x; 
  float a = 1.f - expf(-2.f * 3.1415926f * f * dt); 
  return y + a * (x - y); 
}

static inline float slew_limit(float y, float x, float slew, float dt){
  if (slew <= 0) return x; 
  float d = x - y, m = slew * dt;
  if (d > m) d = m; 
  if (d < -m) d = -m; 
  return y + d;
}

void joystick_tick(){
  static uint32_t t0 = millis(); 
  uint32_t now = millis(); 
  float dt = (now - t0) * 0.001f; 
  if(dt <= 0) return; 
  t0 = now;
  
  joy_cmd.pan   = apply_deadzone_expo(joy_raw.pan,  joy.deadzone, joy.expo);
  joy_cmd.tilt  = apply_deadzone_expo(joy_raw.tilt, joy.deadzone, joy.expo);
  joy_cmd.slide = apply_deadzone_expo(joy_raw.slide, joy.deadzone, joy.expo);

  joy_filt.pan   = slew_limit(joy_filt.pan,   iir_1pole(joy_filt.pan,   joy_cmd.pan,   joy.filt_hz, dt), joy.slew_per_s/(float)PAN_OFFSET_RANGE, dt);
  joy_filt.tilt  = slew_limit(joy_filt.tilt,  iir_1pole(joy_filt.tilt,  joy_cmd.tilt,  joy.filt_hz, dt), joy.slew_per_s/(float)TILT_OFFSET_RANGE, dt);
  joy_filt.slide = slew_limit(joy_filt.slide, iir_1pole(joy_filt.slide, joy_cmd.slide, joy.filt_hz, dt), 1.0f, dt);

  pan_offset_steps  = (long)lroundf(joy_filt.pan  * (float)PAN_OFFSET_RANGE);
  tilt_offset_steps = (long)lroundf(joy_filt.tilt * (float)TILT_OFFSET_RANGE);
  slide_jog_cmd     = clampF(joy_filt.slide, -1.f, +1.f);
}

//==================== NEW: Planificateur "temps commun" ====================
uint32_t pick_duration_ms_for_deltas(const long start[NUM_MOTORS], const long goal[NUM_MOTORS], uint32_t T_req_ms){
  double T = T_req_ms / 1000.0;
  for(;;){
    bool ok = true;
    for(int i=0;i<NUM_MOTORS;i++){
      double d = fabs((double)goal[i] - (double)start[i]);
      double v_need = d * 1.875 / T;          // steps/s
      double a_need = d * 5.7735 / (T*T);     // steps/s^2
      if (v_need > cfg[i].max_speed*0.90 || a_need > cfg[i].max_accel*0.90){
        // augmente T de 10%
        T *= 1.10;
        ok = false;
        break;
      }
    }
    if (ok) break;
  }
  return (uint32_t)lround(T*1000.0);
}

//==================== NEW: Mapping slide->pan/tilt ====================
long pan_comp_from_slide(long slide){
  float u = (float)(slide - cfg[3].min_limit) / (float)(cfg[3].max_limit - cfg[3].min_limit);
  return (long) lround(lerp(PAN_AT_SLIDE_MIN, PAN_AT_SLIDE_MAX, clampF(u,0,1)));
}
long tilt_comp_from_slide(long slide){
  float u = (float)(slide - cfg[3].min_limit) / (float)(cfg[3].max_limit - cfg[3].min_limit);
  return (long) lround(lerp(TILT_AT_SLIDE_MIN, TILT_AT_SLIDE_MAX, clampF(u,0,1)));
}

//==================== NEW: Tick de coordination ====================
void coordinator_tick(){
  static uint32_t last_ms = millis();
  uint32_t now = millis();
  uint32_t dt_ms = now - last_ms;
  if (dt_ms == 0) return;
  last_ms = now;

  // 1) Jog direct Pan/Tilt/Slide (vitesse) quand pas de mouvement sync
  if (!sync_move.active){
    float dt = dt_ms / 1000.0f;
    
    // Jog Pan
    if (fabs(joy_filt.pan) > 0.001f) {
      long p = steppers[0]->targetPos();
      p = clampL(p + (long)lround(joy_filt.pan * PAN_JOG_SPEED * dt), cfg[0].min_limit, cfg[0].max_limit);
      steppers[0]->moveTo(p);
    }
    
    // Jog Tilt
    if (fabs(joy_filt.tilt) > 0.001f) {
      long t = steppers[1]->targetPos();
      t = clampL(t + (long)lround(joy_filt.tilt * TILT_JOG_SPEED * dt), cfg[1].min_limit, cfg[1].max_limit);
      steppers[1]->moveTo(t);
    }
    
    // Jog Slide
    if (fabs(slide_jog_cmd) > 0.001f){
      long s = steppers[3]->targetPos();
      double ds = slide_jog_cmd * SLIDE_JOG_SPEED * dt;
      long goal = clampL(s + (long)lround(ds), cfg[3].min_limit, cfg[3].max_limit);
      steppers[3]->moveTo(goal);
    }
  }

  // 2) Mouvement synchronisé
  if (sync_move.active){
    float tau = (float)(now - sync_move.t0_ms) / (float)sync_move.T_ms;
    if (tau >= 1.0f){
      // Fin de mouvement
      sync_move.active = false;
      tau = 1.0f;
    }
    float s = s_minjerk(tau);

    // Slide de référence (pour couplage)
    long slide_ref = (long)lround( sync_move.start[3] + (sync_move.goal_base[3] - sync_move.start[3]) * s );
    slide_ref = clampL(slide_ref, cfg[3].min_limit, cfg[3].max_limit);

    // Compensations en fonction du slide + offsets joystick (toujours actifs)
    long pan_comp  = pan_comp_from_slide(slide_ref);
    long tilt_comp = tilt_comp_from_slide(slide_ref);

    long pan_goal  = sync_move.goal_base[0] + pan_comp + pan_offset_steps;
    long tilt_goal = sync_move.goal_base[1] + tilt_comp + tilt_offset_steps;
    long zoom_goal = sync_move.goal_base[2];
    long slide_goal= sync_move.goal_base[3];

    // Cibles "à l'instant" suivant s(t)
    long P = (long)lround( sync_move.start[0] + (pan_goal  - sync_move.start[0]) * s );
    long T = (long)lround( sync_move.start[1] + (tilt_goal - sync_move.start[1]) * s );
    long Z = (long)lround( sync_move.start[2] + (zoom_goal - sync_move.start[2]) * s );
    long S = (long)lround( sync_move.start[3] + (slide_goal- sync_move.start[3]) * s );

    // Clip limites
    P = clampL(P, cfg[0].min_limit, cfg[0].max_limit);
    T = clampL(T, cfg[1].min_limit, cfg[1].max_limit);
    Z = clampL(Z, cfg[2].min_limit, cfg[2].max_limit);
    S = clampL(S, cfg[3].min_limit, cfg[3].max_limit);

    // On pousse les cibles. FastAccelStepper replanifie en douceur.
    steppers[0]->moveTo(P);
    steppers[1]->moveTo(T);
    steppers[2]->moveTo(Z);
    steppers[3]->moveTo(S);
  }
}

//==================== Variables globales ====================
long panPos = 0;
long tiltPos = 0;
long zoomPos = 0;
long slidePos = 0;

//==================== OSC ====================
WiFiUDP udp;
OSCErrorCode error;
const int OSC_PORT = 8000;

//==================== Web Server ====================
AsyncWebServer webServer(80);

//==================== Setup OSC ====================
void setupOSC() {
  if (udp.begin(OSC_PORT)) {
    Serial.println("✅ OSC Server started on port " + String(OSC_PORT));
    Serial.println("📡 Waiting for OSC messages...");
  } else {
    Serial.println("❌ Failed to start OSC server on port " + String(OSC_PORT));
  }
}

//==================== Process OSC ====================
void processOSC() {
  OSCMessage msg;
  int size = udp.parsePacket();
  
  if (size > 0) {
    Serial.println("🔔 OSC packet received, size: " + String(size));
    
    while (size--) {
      msg.fill(udp.read());
    }
    
    Serial.println("🔍 OSC message address: " + String(msg.getAddress()));
    Serial.println("🔍 OSC message size: " + String(msg.size()));
    
    if (!msg.hasError()) {
      // Traitement des messages OSC
      // Joystick en OSC (-1..+1)
      msg.dispatch("/pan", [](OSCMessage &m){ 
        joy_raw.pan = clampF(m.getFloat(0), -1.f, +1.f); 
      });
      msg.dispatch("/tilt", [](OSCMessage &m){ 
        joy_raw.tilt = clampF(m.getFloat(0), -1.f, +1.f); 
      });
      msg.dispatch("/joy/pt", [](OSCMessage &m){ 
        joy_raw.pan = clampF(m.getFloat(0), -1.f, +1.f);
        joy_raw.tilt = clampF(m.getFloat(1), -1.f, +1.f); 
      });
      msg.dispatch("/slide/jog", [](OSCMessage &m){ 
        joy_raw.slide = clampF(m.getFloat(0), -1.f, +1.f); 
      });
      
      // Optionnel: réglages runtime
      msg.dispatch("/joy/config", [](OSCMessage &m){
        if (m.size() > 0) joy.deadzone = clampF(m.getFloat(0), 0.f, 0.5f);
        if (m.size() > 1) joy.expo = clampF(m.getFloat(1), 0.f, 0.95f);
        if (m.size() > 2) joy.slew_per_s = fabsf(m.getFloat(2));
        if (m.size() > 3) joy.filt_hz = fabsf(m.getFloat(3));
      });
      
      msg.dispatch("/axis_pan", [](OSCMessage &msg) {
        float value = clampF(msg.getFloat(0), 0.0f, 1.0f);
        long pos_val = (long)(value * (cfg[0].max_limit - cfg[0].min_limit) + cfg[0].min_limit);
        Serial.println("🔧 Moving Pan to: " + String(pos_val));
        steppers[0]->moveTo(pos_val);
        Serial.println("Axis Pan: " + String(value) + " -> " + String(pos_val));
        Serial.println("🔧 Pan stepper running: " + String(steppers[0]->isRunning()));
      });
      
      msg.dispatch("/axis_tilt", [](OSCMessage &msg) {
        float value = clampF(msg.getFloat(0), 0.0f, 1.0f);
        long pos_val = (long)(value * (cfg[1].max_limit - cfg[1].min_limit) + cfg[1].min_limit);
        Serial.println("🔧 Moving Tilt to: " + String(pos_val));
        steppers[1]->moveTo(pos_val);
        Serial.println("Axis Tilt: " + String(value) + " -> " + String(pos_val));
        Serial.println("🔧 Tilt stepper running: " + String(steppers[1]->isRunning()));
      });
      
      msg.dispatch("/axis_zoom", [](OSCMessage &msg) {
        float value = clampF(msg.getFloat(0), 0.0f, 1.0f);
        long pos_val = (long)(value * (cfg[2].max_limit - cfg[2].min_limit) + cfg[2].min_limit);
        Serial.println("🔧 Moving Zoom to: " + String(pos_val));
        steppers[2]->moveTo(pos_val);
        Serial.println("Axis Zoom: " + String(value) + " -> " + String(pos_val));
        Serial.println("🔧 Zoom stepper running: " + String(steppers[2]->isRunning()));
      });
      
      msg.dispatch("/axis_slide", [](OSCMessage &msg) {
        float value = clampF(msg.getFloat(0), 0.0f, 1.0f);
        long pos_val = (long)(value * (cfg[3].max_limit - cfg[3].min_limit) + cfg[3].min_limit);
        Serial.println("🔧 Moving Slide to: " + String(pos_val));
        steppers[3]->moveTo(pos_val);
        Serial.println("Axis Slide: " + String(value) + " -> " + String(pos_val));
        Serial.println("🔧 Slide stepper running: " + String(steppers[3]->isRunning()));
      });
      
      //==================== NEW: Routes OSC avancées ====================
      msg.dispatch("/preset/set", [](OSCMessage &m){
        int i = m.getInt(0);
        presets[i].p = m.getInt(1);
        presets[i].t = m.getInt(2);
        presets[i].z = m.getInt(3);
        presets[i].s = m.getInt(4);
        Serial.printf("Preset %d saved\n", i);
      });

      msg.dispatch("/preset/recall", [](OSCMessage &m){
        int i = m.getInt(0);
        float Tsec = m.getFloat(1); if (Tsec <= 0) Tsec = 2.0f;
        activePreset = i;

        // base goals = preset brut (sans offsets/couplage)
        long base_goal[NUM_MOTORS] = { presets[i].p, presets[i].t, presets[i].z, presets[i].s };
        for(int ax=0; ax<NUM_MOTORS; ++ax){
          sync_move.start[ax]     = steppers[ax]->getCurrentPosition();
          sync_move.goal_base[ax] = clampL(base_goal[ax], cfg[ax].min_limit, cfg[ax].max_limit);
        }
        uint32_t Tms_req = (uint32_t)lround(Tsec*1000.0);
        sync_move.T_ms = pick_duration_ms_for_deltas(sync_move.start, sync_move.goal_base, Tms_req);
        sync_move.t0_ms = millis();
        sync_move.active = true;
        Serial.printf("Recall preset %d in %u ms\n", i, sync_move.T_ms);
      });

      // Note: /pan, /tilt, /slide/jog sont déjà gérés plus haut dans le pipeline joystick

      // Slide: goto [0..1] en T sec (déplacement temps imposé)
      msg.dispatch("/slide/goto", [](OSCMessage &m){
        float u = clampF(m.getFloat(0), 0.0f, 1.0f);
        float Tsec = m.getFloat(1); if (Tsec <= 0) Tsec = 2.0f;

        long s_goal = (long)lround(lerp(cfg[3].min_limit, cfg[3].max_limit, u));
        // Construire un "preset" ad-hoc qui ne bouge que le slide
        for(int ax=0; ax<NUM_MOTORS; ++ax){
          sync_move.start[ax]     = steppers[ax]->getCurrentPosition();
          sync_move.goal_base[ax] = (ax==3) ? s_goal : sync_move.start[ax];
        }
        uint32_t Tms_req = (uint32_t)lround(Tsec*1000.0);
        sync_move.T_ms = pick_duration_ms_for_deltas(sync_move.start, sync_move.goal_base, Tms_req);
        sync_move.t0_ms = millis();
        sync_move.active = true;
      });

      // Config: ranges offsets et mapping slide->pan/tilt
      msg.dispatch("/config/offset_range", [](OSCMessage &m){
        PAN_OFFSET_RANGE  = m.getInt(0);
        TILT_OFFSET_RANGE = m.getInt(1);
      });
      msg.dispatch("/config/pan_map", [](OSCMessage &m){
        PAN_AT_SLIDE_MIN = m.getInt(0);
        PAN_AT_SLIDE_MAX = m.getInt(1);
      });
      msg.dispatch("/config/tilt_map", [](OSCMessage &m){
        TILT_AT_SLIDE_MIN = m.getInt(0);
        TILT_AT_SLIDE_MAX = m.getInt(1);
      });
    } else {
      Serial.println("❌ OSC Error: " + String(msg.getError()));
    }
  }
}

//==================== Web Handlers ====================
void setupWebServer() {
  webServer.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(200, "text/plain", "ESP32 Slider Controller - OSC Server Running");
  });
  
  webServer.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
    String status = "Pan: " + String(panPos) + " Tilt: " + String(tiltPos) + 
                   " Zoom: " + String(zoomPos) + " Slide: " + String(slidePos);
    request->send(200, "text/plain", status);
  });
  
  webServer.begin();
  Serial.println("🌐 Web server started");
}

//==================== Setup ====================
void setup() {
  Serial.begin(115200);
  delay(200);
  
  Serial.println("🚀 ESP32 Slider Controller Starting...");
  
  // Initialiser l'engine
  engine.init();
  
  // Configurer les drivers TMC
  setupDriversTMC();
  
  // Attacher les steppers
  for (int i=0; i<NUM_MOTORS; i++) {
    steppers[i] = engine.stepperConnectToPin(STEP_PINS[i]);
    if (steppers[i]) {
      Serial.println("✅ Stepper " + String(i) + " connected to pin " + String(STEP_PINS[i]));
      steppers[i]->setDirectionPin(DIR_PINS[i]);
      steppers[i]->setEnablePin(ENABLE_PINS[i], true);   // true = active LOW pour TMC2209
      steppers[i]->setAutoEnable(false);                 // Garde les moteurs alimentés
      steppers[i]->setSpeedInHz(cfg[i].max_speed);
      steppers[i]->setAcceleration(cfg[i].max_accel);
      steppers[i]->enableOutputs();                      // Force l'activation maintenant
      Serial.println("✅ Stepper " + String(i) + " configured");
    } else {
      Serial.println("❌ Failed to connect stepper " + String(i));
    }
  }
  
  // WiFi Manager
  WiFiManager wm;
  wm.autoConnect("ESP32-Slider");
  
  Serial.println("📡 WiFi connected: " + WiFi.localIP().toString());
  
  // OTA
  ArduinoOTA.begin();
  
  // Web Server
  setupWebServer();
  
  // OSC
  setupOSC();
  
  Serial.println("🎯 System ready!");
}

//==================== Loop ====================
void loop() {
  ArduinoOTA.handle();
  processOSC();
  joystick_tick();     // NEW: Pipeline joystick avec lissage
  coordinator_tick();  // NEW: Orchestrateur de mouvements synchronisés
  
  // FastAccelStepper n'a pas besoin de engine.run()
  // Les moteurs se déplacent automatiquement
  
  // Mettre à jour les positions
  panPos = steppers[0]->getCurrentPosition();
  tiltPos = steppers[1]->getCurrentPosition();
  zoomPos = steppers[2]->getCurrentPosition();
  slidePos = steppers[3]->getCurrentPosition();
  
  // Log périodique (console web + série)
  static unsigned long tlog = 0;
  static unsigned long osc_log = 0;
  if (millis() - tlog > 500) {
    tlog = millis();
    String s = "t=" + String(millis()/1000.0, 2) + " jog=" + String(slide_jog_cmd, 2) +
               " | P:" + String(panPos) + " T:" + String(tiltPos) +
               " Z:" + String(zoomPos) + " S:" + String(slidePos);
    Serial.println(s);
  }
  
  // Log OSC status toutes les 5 secondes
  if (millis() - osc_log > 5000) {
    osc_log = millis();
    Serial.println("🔍 OSC listening on port " + String(OSC_PORT));
    
    // Vérifier l'état des drivers TMC
    for (int i=0; i<NUM_MOTORS; i++) {
      Serial.println("🔧 Driver " + String(i) + " toff: " + String(drivers[i]->toff()) + 
                     " tstep: " + String(drivers[i]->TSTEP()));
    }
  }
}