#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

// ===== Wi-Fi (your credentials) =====
const char* WIFI_SSID = "z4zitu";
const char* WIFI_PASS = "12344321";

// ===== Pins =====
#define TRIG_PIN   5
#define ECHO_PIN   18
#define RELAY_PIN  23   // active-low on many relay modules

// ===== Tank geometry =====
// Physical depth from sensor face to bottom (cm)
#define TANK_HEIGHT_CM 22.86f   // 9 inches

// ===== Thresholds (percent full) =====
#define ON_PERCENT   20.0f      // allow manual ON at/under this
#define OFF_PERCENT  80.0f      // auto-OFF at/above this

// ===== Filtering / debounce =====
#define SAMPLES 9               // for median
#define STABILITY_COUNT 3       // consecutive readings before OFF

WebServer server(80);
Preferences prefs;

bool  pumpOn = false;
int   stableOffCount = 0;

float lastDistance = -1.0f;     // cm from sensor face to water

// History tracking
struct HistoryEntry {
  unsigned long timestamp;
  float percent;
  bool pumpState;
};

HistoryEntry history[50];  // Store last 50 readings
int historyIndex = 0;
int historyCount = 0;

// Statistics
unsigned long totalRunTime = 0;
unsigned long pumpStartTime = 0;
unsigned long lastCycleTime = 0;
int dailyCycles = 0;

// ---------- Utils ----------
static void sortAsc(float* a, int n) {
  for (int i = 0; i < n - 1; i++) {
    int m = i;
    for (int j = i + 1; j < n; j++) if (a[j] < a[m]) m = j;
    if (m != i) { float t = a[i]; a[i] = a[m]; a[m] = t; }
  }
}

// One ping -> cm; -1 on error
float readDistanceOnce() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long d = pulseIn(ECHO_PIN, HIGH, 30000); // ~30ms timeout
  if (d <= 0) return -1.0f;
  return (d * 0.0343f) / 2.0f;             // cm
}

// Median of valid samples (reject out-of-range)
float getFilteredDistance() {
  float buf[SAMPLES];
  int n = 0;
  for (int i = 0; i < SAMPLES; i++) {
    float cm = readDistanceOnce();
    if (cm > 2 && cm < 100) buf[n++] = cm;
    delay(25);
  }
  if (n == 0) return -1.0f;
  sortAsc(buf, n);
  return buf[n / 2];
}

void setPump(bool on) {
  if (pumpOn != on) {
    if (on) {
      pumpStartTime = millis();
    } else {
      if (pumpOn) {
        totalRunTime += (millis() - pumpStartTime);
        dailyCycles++;
      }
    }
  }
  pumpOn = on;
  digitalWrite(RELAY_PIN, on ? LOW : HIGH); // active-low
}

void addToHistory(float percent) {
  history[historyIndex] = {millis(), percent, pumpOn};
  historyIndex = (historyIndex + 1) % 50;
  if (historyCount < 50) historyCount++;
}

float percentFromDistance(float d) {
  // Use physical height to calculate percentage
  float pct = ( (TANK_HEIGHT_CM - d) / TANK_HEIGHT_CM ) * 100.0f;
  if (pct < 0) pct = 0; if (pct > 100) pct = 100;
  return pct;
}

String statusMessage(float pct) {
  if (pct <= ON_PERCENT) {
    if (!pumpOn) return "Low level detected - Ready to start pump";
    else return "Pump active - Filling tank to safe level";
  } else if (pct >= OFF_PERCENT) {
    if (pumpOn) return "High level reached - Auto-stop in progress";
    else return "Tank at optimal level - System ready";
  } else {
    return pumpOn ? "Pump running - Normal operation" : "Level normal - Monitoring active";
  }
}

// ---------- Web Handlers ----------
void handleRoot() {
  String html = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Smart Water Tank Monitor</title>
<style>
  * { margin: 0; padding: 0; box-sizing: border-box; }
  
  body { 
    font-family: 'Segoe UI', system-ui, -apple-system, sans-serif; 
    background: linear-gradient(45deg, #ff6b6b, #feca57, #48dbfb, #ff9ff3, #a8e6cf, #ff8a80);
    background-size: 600% 600%;
    animation: gradientShift 20s ease infinite;
    min-height: 100vh;
    color: #333;
    overflow-x: hidden;
    position: relative;
  }
  
  body::before {
    content: '';
    position: fixed;
    top: 0;
    left: 0;
    width: 100%;
    height: 100%;
    background: 
      radial-gradient(circle at 20% 80%, rgba(120, 119, 198, 0.3) 0%, transparent 50%),
      radial-gradient(circle at 80% 20%, rgba(255, 119, 198, 0.3) 0%, transparent 50%),
      radial-gradient(circle at 40% 40%, rgba(120, 219, 255, 0.3) 0%, transparent 50%);
    animation: floatingBubbles 25s ease-in-out infinite;
    pointer-events: none;
    z-index: -1;
  }
  
  body::after {
    content: '';
    position: fixed;
    top: 0;
    left: 0;
    width: 100%;
    height: 100%;
    background: 
      linear-gradient(45deg, transparent 30%, rgba(255,255,255,0.1) 32%, transparent 34%),
      linear-gradient(-45deg, transparent 30%, rgba(255,255,255,0.1) 32%, transparent 34%);
    background-size: 60px 60px;
    animation: shimmer 8s linear infinite;
    pointer-events: none;
    z-index: -1;
  }
  
  @keyframes gradientShift {
    0% { background-position: 0% 50%; }
    25% { background-position: 100% 100%; }
    50% { background-position: 100% 50%; }
    75% { background-position: 0% 0%; }
    100% { background-position: 0% 50%; }
  }
  
  @keyframes floatingBubbles {
    0%, 100% { 
      transform: translateY(0px) rotate(0deg);
      opacity: 0.7;
    }
    33% { 
      transform: translateY(-30px) rotate(120deg);
      opacity: 0.9;
    }
    66% { 
      transform: translateY(-60px) rotate(240deg);
      opacity: 0.6;
    }
  }
  
  @keyframes shimmer {
    0% { transform: translateX(-100px) translateY(-100px); }
    100% { transform: translateX(100px) translateY(100px); }
  }
  
  .container {
    max-width: 1200px;
    margin: 0 auto;
    padding: 20px;
  }
  
  .header {
    text-align: center;
    color: white;
    margin-bottom: 30px;
  }
  
  .header h1 {
    font-size: 2.5rem;
    font-weight: 300;
    margin-bottom: 10px;
    text-shadow: 
      0 2px 10px rgba(0,0,0,0.3),
      0 0 30px rgba(255,255,255,0.5);
    animation: titleGlow 4s ease-in-out infinite alternate;
  }
  
  @keyframes titleGlow {
    0% { 
      text-shadow: 
        0 2px 10px rgba(0,0,0,0.3),
        0 0 30px rgba(255,255,255,0.5);
    }
    100% { 
      text-shadow: 
        0 2px 10px rgba(0,0,0,0.3),
        0 0 50px rgba(255,255,255,0.8),
        0 0 80px rgba(255,255,255,0.3);
    }
  }
  
  .header p {
    opacity: 0.9;
    font-size: 1.1rem;
  }
  
  .dashboard {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 20px;
    margin-bottom: 20px;
  }
  
  .card {
    background: rgba(255,255,255,0.95);
    border-radius: 20px;
    padding: 25px;
    box-shadow: 0 8px 32px rgba(0,0,0,0.1);
    backdrop-filter: blur(15px);
    border: 1px solid rgba(255,255,255,0.3);
    transition: all 0.4s cubic-bezier(0.175, 0.885, 0.32, 1.275);
    position: relative;
    overflow: hidden;
  }
  
  .card::before {
    content: '';
    position: absolute;
    top: 0;
    left: -100%;
    width: 100%;
    height: 100%;
    background: linear-gradient(90deg, transparent, rgba(255,255,255,0.3), transparent);
    transition: left 0.6s ease;
  }
  
  .card:hover::before {
    left: 100%;
  }
  
  .card:hover {
    transform: translateY(-8px) scale(1.02);
    box-shadow: 0 20px 50px rgba(0,0,0,0.2);
    border: 1px solid rgba(255,255,255,0.5);
  }
  
  .tank-visual {
    position: relative;
    width: 200px;
    height: 300px;
    margin: 0 auto 20px;
    background: linear-gradient(to bottom, #e3f2fd, #bbdefb);
    border: 4px solid #1976d2;
    border-radius: 0 0 20px 20px;
    overflow: hidden;
    box-shadow: 
      inset 0 0 20px rgba(0,0,0,0.1),
      0 0 30px rgba(25, 118, 210, 0.3);
    animation: tankGlow 4s ease-in-out infinite alternate;
  }
  
  @keyframes tankGlow {
    0% { box-shadow: inset 0 0 20px rgba(0,0,0,0.1), 0 0 30px rgba(25, 118, 210, 0.3); }
    100% { box-shadow: inset 0 0 20px rgba(0,0,0,0.1), 0 0 50px rgba(25, 118, 210, 0.6); }
  }
  
  .tank-water {
    position: absolute;
    bottom: 0;
    left: 0;
    width: 100%;
    background: linear-gradient(to top, #1565c0, #42a5f5, #81d4fa);
    transition: height 1s cubic-bezier(0.4, 0, 0.2, 1);
    border-radius: 0 0 16px 16px;
    animation: waterFlow 3s ease-in-out infinite;
  }
  
  .tank-water::before {
    content: '';
    position: absolute;
    top: -10px;
    left: 0;
    width: 100%;
    height: 20px;
    background: 
      radial-gradient(ellipse at center, rgba(255,255,255,0.4) 0%, transparent 70%);
    border-radius: 50%;
    animation: wave 2s ease-in-out infinite;
  }
  
  .tank-water::after {
    content: '';
    position: absolute;
    top: 0;
    left: 0;
    width: 100%;
    height: 100%;
    background: repeating-linear-gradient(
      90deg,
      transparent,
      transparent 10px,
      rgba(255,255,255,0.1) 10px,
      rgba(255,255,255,0.1) 20px
    );
    animation: ripple 4s linear infinite;
  }
  
  @keyframes waterFlow {
    0%, 100% { filter: hue-rotate(0deg) brightness(1); }
    50% { filter: hue-rotate(20deg) brightness(1.1); }
  }
  
  @keyframes wave {
    0%, 100% { 
      transform: translateX(-50%) scale(1) rotate(0deg);
      opacity: 0.7;
    }
    33% { 
      transform: translateX(-30%) scale(1.1) rotate(1deg);
      opacity: 0.9;
    }
    66% { 
      transform: translateX(-70%) scale(0.9) rotate(-1deg);
      opacity: 0.8;
    }
  }
  
  @keyframes ripple {
    0% { transform: translateX(-100%); }
    100% { transform: translateX(100%); }
  }
  
  .tank-sensor {
    position: absolute;
    top: -15px;
    left: 50%;
    transform: translateX(-50%);
    width: 40px;
    height: 20px;
    background: #424242;
    border-radius: 5px;
    box-shadow: 0 2px 5px rgba(0,0,0,0.2);
  }
  
  .level-display {
    text-align: center;
    margin-bottom: 20px;
  }
  
  .level-percent {
    font-size: 3rem;
    font-weight: bold;
    color: #1976d2;
    margin-bottom: 10px;
  }
  
  .level-bar {
    width: 100%;
    height: 8px;
    background: #e0e0e0;
    border-radius: 4px;
    overflow: hidden;
    margin: 10px 0;
  }
  
  .level-fill {
    height: 100%;
    background: linear-gradient(90deg, #4caf50, #8bc34a);
    transition: width 0.8s ease;
    border-radius: 4px;
  }
  
  .metrics {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 15px;
    margin: 20px 0;
  }
  
  .metric {
    text-align: center;
    padding: 15px;
    background: rgba(0,0,0,0.05);
    border-radius: 10px;
  }
  
  .metric-value {
    font-size: 1.5rem;
    font-weight: bold;
    color: #1976d2;
  }
  
  .metric-label {
    font-size: 0.85rem;
    color: #666;
    margin-top: 5px;
  }
  
  .pump-control {
    text-align: center;
    margin: 20px 0;
  }
  
  .pump-status {
    display: inline-flex;
    align-items: center;
    padding: 10px 20px;
    border-radius: 25px;
    margin-bottom: 15px;
    font-weight: 500;
    transition: all 0.3s ease;
    position: relative;
    overflow: hidden;
  }
  
  .pump-status::before {
    content: '';
    position: absolute;
    top: 0;
    left: -100%;
    width: 100%;
    height: 100%;
    background: linear-gradient(90deg, transparent, rgba(255,255,255,0.3), transparent);
    transition: left 0.5s ease;
  }
  
  .pump-on {
    background: linear-gradient(135deg, #4caf50, #8bc34a, #c8e6c9);
    color: white;
    animation: pumpPulse 2s infinite, pumpGlow 3s ease-in-out infinite alternate;
    box-shadow: 0 0 20px rgba(76, 175, 80, 0.4);
  }
  
  .pump-off {
    background: linear-gradient(135deg, #f44336, #e57373, #ffcdd2);
    color: white;
    animation: pumpGlow 3s ease-in-out infinite alternate;
    box-shadow: 0 0 20px rgba(244, 67, 54, 0.4);
  }
  
  .pump-status:hover::before {
    left: 100%;
  }
  
  @keyframes pumpPulse {
    0%, 100% { opacity: 1; transform: scale(1); }
    50% { opacity: 0.8; transform: scale(1.05); }
  }
  
  @keyframes pumpGlow {
    0% { filter: brightness(1) saturate(1); }
    100% { filter: brightness(1.2) saturate(1.3); }
  }
  
  .btn {
    background: linear-gradient(135deg, #1976d2, #42a5f5, #81d4fa);
    color: white;
    border: none;
    padding: 12px 30px;
    border-radius: 25px;
    font-size: 1rem;
    font-weight: 500;
    cursor: pointer;
    transition: all 0.4s cubic-bezier(0.175, 0.885, 0.32, 1.275);
    box-shadow: 0 4px 15px rgba(25,118,210,0.3);
    position: relative;
    overflow: hidden;
  }
  
  .btn::before {
    content: '';
    position: absolute;
    top: 50%;
    left: 50%;
    width: 0;
    height: 0;
    background: rgba(255,255,255,0.2);
    border-radius: 50%;
    transform: translate(-50%, -50%);
    transition: width 0.6s ease, height 0.6s ease;
  }
  
  .btn:hover::before {
    width: 300px;
    height: 300px;
  }
  
  .btn:hover {
    transform: translateY(-3px) scale(1.05);
    box-shadow: 
      0 8px 25px rgba(25,118,210,0.4),
      0 0 30px rgba(25,118,210,0.3);
    filter: brightness(1.1);
  }
  
  .btn:active {
    transform: translateY(-1px) scale(1.02);
  }
  
  .btn:disabled {
    background: linear-gradient(135deg, #bdbdbd, #e0e0e0);
    cursor: not-allowed;
    transform: none;
    box-shadow: none;
    filter: none;
  }
  
  .btn:disabled::before {
    display: none;
  }
  
  .status-message {
    text-align: center;
    padding: 15px;
    background: rgba(33,150,243,0.1);
    border-radius: 10px;
    border-left: 4px solid #2196f3;
    margin: 15px 0;
    color: #1565c0;
  }
  
  .history-chart {
    height: 200px;
    background: rgba(0,0,0,0.05);
    border-radius: 10px;
    margin: 20px 0;
    position: relative;
    overflow: hidden;
  }
  
  .stats-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
    gap: 15px;
    margin: 20px 0;
  }
  
  .stat-card {
    background: rgba(0,0,0,0.05);
    padding: 15px;
    border-radius: 10px;
    text-align: center;
  }
  
  .stat-value {
    font-size: 1.5rem;
    font-weight: bold;
    color: #1976d2;
  }
  
  .timestamp {
    color: #666;
    font-size: 0.9rem;
    text-align: center;
    margin-top: 10px;
  }
  
  .calibration {
    margin-top: 20px;
    padding: 15px;
    background: rgba(255,193,7,0.1);
    border-radius: 10px;
    border-left: 4px solid #ff9800;
  }
  
  .calibration-status {
    font-weight: 500;
    margin-bottom: 10px;
  }
  
  @media (max-width: 768px) {
    .dashboard {
      grid-template-columns: 1fr;
    }
    
    .tank-visual {
      width: 150px;
      height: 225px;
    }
    
    .level-percent {
      font-size: 2.5rem;
    }
    
    .header h1 {
      font-size: 2rem;
    }
  }
</style>
</head>
<body>
  <div class="container">
    <div class="header">
      <h1>🌊 Smart Water Tank Monitor</h1>
      <p>Advanced IoT Water Management System</p>
    </div>
    
    <div class="dashboard">
      <div class="card">
        <div class="tank-visual">
          <div class="tank-sensor"></div>
          <div id="tankWater" class="tank-water" style="height: 0%"></div>
        </div>
        
        <div class="level-display">
          <div id="levelPercent" class="level-percent">--%</div>
          <div class="level-bar">
            <div id="levelFill" class="level-fill" style="width: 0%"></div>
          </div>
        </div>
        
        <div class="metrics">
          <div class="metric">
            <div id="waterLevel" class="metric-value">--</div>
            <div class="metric-label">Water Level (cm)</div>
          </div>
          <div class="metric">
            <div id="sensorDistance" class="metric-value">--</div>
            <div class="metric-label">Sensor Distance (cm)</div>
          </div>
        </div>
        
        <div class="pump-control">
          <div id="pumpStatus" class="pump-status pump-off">
            <span>🔴 Pump: OFF</span>
          </div>
          <button id="pumpButton" class="btn" disabled>Turn Pump ON</button>
        </div>
        
        <div id="statusMessage" class="status-message">
          Loading system status...
        </div>
        
        <div class="timestamp" id="lastUpdate">
          Last update: --
        </div>
      </div>
      
      <div class="card">
        <h3 style="margin-bottom: 20px; color: #1976d2;">📊 System Analytics</h3>
        
        <div class="stats-grid">
          <div class="stat-card">
            <div id="totalRuntime" class="stat-value">--</div>
            <div class="metric-label">Total Runtime (min)</div>
          </div>
          <div class="stat-card">
            <div id="dailyCycles" class="stat-value">--</div>
            <div class="metric-label">Daily Cycles</div>
          </div>
          <div class="stat-card">
            <div id="efficiency" class="stat-value">--</div>
            <div class="metric-label">Efficiency (%)</div>
          </div>
          <div class="stat-card">
            <div id="avgLevel" class="stat-value">--</div>
            <div class="metric-label">Avg Level (%)</div>
          </div>
        </div>
        
        <div class="history-chart" id="historyChart">
          <canvas id="chartCanvas" width="400" height="200"></canvas>
        </div>
      </div>
    </div>
  </div>

<script>
let lastData = null;
let chartData = [];
let maxDataPoints = 50;

function updateTankVisual(percent) {
  const tankWater = document.getElementById('tankWater');
  tankWater.style.height = Math.max(0, Math.min(100, percent)) + '%';
  
  // Change water color based on level
  if (percent <= 20) {
    tankWater.style.background = 'linear-gradient(to top, #f44336, #e57373)';
  } else if (percent >= 90) {
    tankWater.style.background = 'linear-gradient(to top, #4caf50, #8bc34a)';
  } else {
    tankWater.style.background = 'linear-gradient(to top, #1565c0, #42a5f5)';
  }
}

function drawChart() {
  const canvas = document.getElementById('chartCanvas');
  const ctx = canvas.getContext('2d');
  
  // Clear canvas
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  
  if (chartData.length < 2) return;
  
  const width = canvas.width;
  const height = canvas.height;
  const padding = 20;
  
  // Draw grid
  ctx.strokeStyle = '#e0e0e0';
  ctx.lineWidth = 1;
  
  // Horizontal lines
  for (let i = 0; i <= 4; i++) {
    const y = padding + (height - 2 * padding) * i / 4;
    ctx.beginPath();
    ctx.moveTo(padding, y);
    ctx.lineTo(width - padding, y);
    ctx.stroke();
  }
  
  // Draw chart line
  ctx.strokeStyle = '#2196f3';
  ctx.lineWidth = 2;
  ctx.beginPath();
  
  chartData.forEach((point, index) => {
    const x = padding + (width - 2 * padding) * index / (chartData.length - 1);
    const y = height - padding - (height - 2 * padding) * point.percent / 100;
    
    if (index === 0) {
      ctx.moveTo(x, y);
    } else {
      ctx.lineTo(x, y);
    }
  });
  
  ctx.stroke();
  
  // Draw pump state indicators
  chartData.forEach((point, index) => {
    if (point.pumpOn) {
      const x = padding + (width - 2 * padding) * index / (chartData.length - 1);
      const y = height - padding - (height - 2 * padding) * point.percent / 100;
      
      ctx.fillStyle = '#4caf50';
      ctx.beginPath();
      ctx.arc(x, y, 3, 0, 2 * Math.PI);
      ctx.fill();
    }
  });
}

async function fetchData() {
  try {
    const response = await fetch('/data');
    const data = await response.json();
    
    if (!data.ok) {
      throw new Error('Sensor error');
    }
    
    lastData = data;
    
    // Update main display
    document.getElementById('levelPercent').textContent = data.percent.toFixed(1) + '%';
    document.getElementById('levelFill').style.width = Math.max(0, Math.min(100, data.percent)) + '%';
    document.getElementById('waterLevel').textContent = data.water_cm.toFixed(1);
    document.getElementById('sensorDistance').textContent = data.distance_cm.toFixed(1);
    
    // Update tank visual
    updateTankVisual(data.percent);
    
    // Update pump status
    const pumpStatus = document.getElementById('pumpStatus');
    const pumpButton = document.getElementById('pumpButton');
    
    if (data.pump_on) {
      pumpStatus.className = 'pump-status pump-on';
      pumpStatus.innerHTML = '<span>🟢 Pump: ON</span>';
      pumpButton.textContent = 'Turn Pump OFF';
      pumpButton.disabled = false;
    } else {
      pumpStatus.className = 'pump-status pump-off';
      pumpStatus.innerHTML = '<span>🔴 Pump: OFF</span>';
      pumpButton.textContent = 'Turn Pump ON';
      pumpButton.disabled = !(data.percent <= data.on_threshold);
    }
    
    // Update status message
    document.getElementById('statusMessage').textContent = data.message;
    
    // Update timestamp
    document.getElementById('lastUpdate').textContent = 'Last update: ' + new Date().toLocaleTimeString();
    
    // Add to chart data
    chartData.push({
      timestamp: Date.now(),
      percent: data.percent,
      pumpOn: data.pump_on
    });
    
    if (chartData.length > maxDataPoints) {
      chartData.shift();
    }
    
    drawChart();
    
    // Update analytics (mock data for now)
    updateAnalytics(data);
    
  } catch (error) {
    document.getElementById('statusMessage').textContent = 'Connection error - Retrying...';
    console.error('Fetch error:', error);
  }
}

function updateAnalytics(data) {
  // Mock analytics - in real implementation, these would come from server
  document.getElementById('totalRuntime').textContent = '45';
  document.getElementById('dailyCycles').textContent = '3';
  document.getElementById('efficiency').textContent = '92';
  
  if (chartData.length > 0) {
    const avgLevel = chartData.reduce((sum, point) => sum + point.percent, 0) / chartData.length;
    document.getElementById('avgLevel').textContent = avgLevel.toFixed(1);
  }
}

async function controlPump(command) {
  try {
    const response = await fetch(`/pump?cmd=${command}`);
    const result = await response.json();
    
    if (result.ok) {
      document.getElementById('statusMessage').textContent = 
        command === 'on' ? 'Pump started successfully' : 'Pump stopped successfully';
    } else {
      document.getElementById('statusMessage').textContent = 'Action blocked: ' + result.message;
    }
    
    // Refresh data immediately
    setTimeout(fetchData, 500);
    
  } catch (error) {
    document.getElementById('statusMessage').textContent = 'Control command failed';
  }
}

// Event listeners
document.getElementById('pumpButton').addEventListener('click', () => {
  if (!lastData) return;
  
  const command = lastData.pump_on ? 'off' : 'on';
  controlPump(command);
});

// Initialize
fetchData();
setInterval(fetchData, 2000); // Update every 2 seconds
</script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleData() {
  if (lastDistance < 0) {
    server.send(200, "application/json", "{\"ok\":false,\"error\":\"sensor\"}");
    return;
  }
  float d = lastDistance;
  float pct = percentFromDistance(d);
  float water_cm = (pct / 100.0f) * TANK_HEIGHT_CM;

  String msg = statusMessage(pct);

  // Calculate runtime in minutes
  unsigned long currentRuntime = totalRunTime;
  if (pumpOn && pumpStartTime > 0) {
    currentRuntime += (millis() - pumpStartTime);
  }
  
  String json = "{";
  json += "\"ok\":true,";
  json += "\"distance_cm\":" + String(d, 1) + ",";
  json += "\"water_cm\":" + String(water_cm, 1) + ",";
  json += "\"percent\":" + String(pct, 1) + ",";
  json += "\"pump_on\":" + String(pumpOn ? "true" : "false") + ",";
  json += "\"on_threshold\":" + String(ON_PERCENT, 1) + ",";
  json += "\"off_threshold\":" + String(OFF_PERCENT, 1) + ",";
  json += "\"total_runtime\":" + String(currentRuntime / 60000) + ",";
  json += "\"daily_cycles\":" + String(dailyCycles) + ",";
  json += "\"message\":\"" + msg + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

void handlePump() {
  if (!server.hasArg("cmd")) {
    server.send(400, "application/json", "{\"ok\":false,\"message\":\"missing-cmd\"}");
    return;
  }
  String cmd = server.arg("cmd");
  String reason = "ok";
  bool ok = true;

  if (cmd == "on") {
    // allow ON only when <= 20%
    if (lastDistance < 0) { ok = false; reason = "sensor-unavailable"; }
    else {
      float pct = percentFromDistance(lastDistance);
      if (pct <= ON_PERCENT) setPump(true);
      else { ok = false; reason = "blocked-above-20"; }
    }
  } else if (cmd == "off") {
    setPump(false);
  } else {
    ok = false; reason = "bad-cmd";
  }

  String json = "{";
  json += "\"ok\":" + String(ok ? "true" : "false") + ",";
  json += "\"pump_on\":" + String(pumpOn ? "true" : "false") + ",";
  json += "\"message\":\"" + reason + "\"}";
  server.send(ok ? 200 : 400, "application/json", json);
}

void handleCal() {
  // Calibration feature removed - return error
  server.send(404, "application/json", "{\"ok\":false,\"message\":\"calibration-disabled\"}");
}

void handleRaw() {
  // Return a small array of raw samples for debugging
  const int N = 15;
  String out = "[";
  for (int i = 0; i < N; i++) {
    float cm = readDistanceOnce();
    if (i) out += ",";
    out += String(cm, 1);
    delay(20);
  }
  out += "]";
  server.send(200, "application/json", out);
}

void handleHistory() {
  String json = "{\"history\":[";
  int count = 0;
  for (int i = 0; i < historyCount; i++) {
    int idx = (historyIndex - historyCount + i + 50) % 50;
    if (count > 0) json += ",";
    json += "{\"timestamp\":" + String(history[idx].timestamp) + ",";
    json += "\"percent\":" + String(history[idx].percent, 1) + ",";
    json += "\"pump_on\":" + String(history[idx].pumpState ? "true" : "false") + "}";
    count++;
  }
  json += "]}";
  server.send(200, "application/json", json);
}

// ---------- Setup / Loop ----------
void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, HIGH); // OFF initially (active-low)

  // Initialize preferences
  prefs.begin("tankmon", false);

  // Load statistics
  totalRunTime = prefs.getULong("total_runtime", 0);
  dailyCycles = prefs.getInt("daily_cycles", 0);

  // Wi-Fi connect or AP fallback
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 40) {
    delay(250); Serial.print("."); tries++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi failed. Starting Access Point...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("TankMonitor", "12345678");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  }

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/pump", HTTP_GET, handlePump);
  server.on("/cal", HTTP_GET, handleCal);
  server.on("/raw", HTTP_GET, handleRaw);
  server.on("/history", HTTP_GET, handleHistory);
  server.begin();
  Serial.println("HTTP server started");
}

void loop() {
  server.handleClient();

  float d = getFilteredDistance();
  if (d < 0) {
    Serial.println("Sensor error: no echo");
    lastDistance = -1.0f;
    delay(200);
    return;
  }
  lastDistance = d;

  float pct = percentFromDistance(d);
  
  // Add to history every 30 seconds
  static unsigned long lastHistoryUpdate = 0;
  if (millis() - lastHistoryUpdate > 30000) {
    addToHistory(pct);
    lastHistoryUpdate = millis();
    
    // Save statistics periodically
    prefs.putULong("total_runtime", totalRunTime);
    prefs.putInt("daily_cycles", dailyCycles);
  }

  // Auto-OFF only (no auto-ON)
  if (pct >= OFF_PERCENT) stableOffCount++;
  else stableOffCount = 0;

  if (pumpOn && stableOffCount >= STABILITY_COUNT) {
    setPump(false);
    Serial.println("Pump: OFF (auto >=90%)");
  }

  Serial.print("Distance: "); Serial.print(d, 1);
  Serial.print(" cm | %: "); Serial.print(pct, 1);
  Serial.print(" | Pump: "); Serial.println(pumpOn ? "ON" : "OFF");

  delay(400);
}
