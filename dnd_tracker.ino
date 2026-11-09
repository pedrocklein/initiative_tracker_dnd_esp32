#include <WiFi.h>
#include <WebServer.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include <FS.h>
#include <SD.h>
#include <vector>
#include <algorithm>

// --- Declarações Prévias de Funções ---
void handleNext();
void handlePrev();
void handleStart();
void handleEnd();
void updateDisplay();

// --- Credenciais Padrão de Wi-Fi ---
const char* default_ssid = "Pedro";
const char* default_pass = "internetico";

// --- Mapeamento de Pinos da Placa CYD ---
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33

#define SD_CS        5
#define SD_MOSI      23
#define SD_MISO      19
#define SD_SCK       18

#define TFT_BL       21
#define PIN_BTN_BOOT 0

// Instâncias
TFT_eSPI tft = TFT_eSPI();
SPIClass touchSpi = SPIClass(HSPI);
SPIClass sdSPI = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
WebServer server(80);
Preferences prefs;

bool sdAvailable = false;
File uploadFile;

struct Combatant {
  String name;
  String icon;
  int initiative;
  String image;
  bool active;
};

std::vector<Combatant> combatants;
bool inCombat = false;
int currentIndex = 0;
int currentRound = 1;
unsigned long lastActionTime = 0;
bool isApMode = false;

// --- Variáveis do Timer ---
bool timerRunning = false;
unsigned long timerEndMillis = 0;
bool timerExpired = false;
unsigned long timerExpiredMillis = 0;
unsigned long lastTimerSecondUpdate = 0;

// --- Leitura Segura de Inteiros Little-Endian ---
inline uint32_t readLE32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

inline uint16_t readLE16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

// --- Leitor de Imagem BMP 24-bit Otimizado e Seguro ---
bool drawBmp(const char *filename, int x, int y) {
  if (!sdAvailable || filename == NULL || strlen(filename) == 0) return false;

  String path = filename;
  if (!path.startsWith("/")) path = "/" + path;

  int dot = path.lastIndexOf('.');
  if (dot != -1) {
    String ext = path.substring(dot);
    if (ext.equalsIgnoreCase(".png") || ext.equalsIgnoreCase(".jpg") || ext.equalsIgnoreCase(".jpeg")) {
      path = path.substring(0, dot) + ".bmp";
    }
  }

  File bmpFile = SD.open(path);
  if (!bmpFile) {
    String pathInImages = "/images" + path;
    bmpFile = SD.open(pathInImages);
    if (!bmpFile) {
      Serial.printf("[BMP] Nao encontrou: %s nem %s\n", path.c_str(), pathInImages.c_str());
      return false;
    }
  }

  uint8_t header[54];
  if (bmpFile.read(header, 54) != 54) {
    bmpFile.close();
    return false;
  }

  bool validMagic = (header[0] == 'B' && header[1] == 'M') || (header[0] == 'M' && header[1] == 'B');
  if (!validMagic) {
    Serial.printf("[BMP] Assinatura invalida: %02X %02X\n", header[0], header[1]);
    bmpFile.close();
    return false;
  }

  uint32_t imageOffset = readLE32(&header[10]);
  int32_t width        = (int32_t)readLE32(&header[18]);
  int32_t height       = (int32_t)readLE32(&header[22]);
  uint16_t depth       = readLE16(&header[28]);
  uint32_t compression= readLE32(&header[30]);

  if (depth != 24 || compression != 0 || width <= 0) {
    Serial.printf("[BMP] Nao suportado: depth=%d comp=%d w=%d\n", depth, compression, width);
    bmpFile.close();
    return false;
  }

  bool flip = true;
  if (height < 0) {
    height = -height;
    flip = false;
  }

  if (width > 240 || height > 240) {
    bmpFile.close();
    return false;
  }

  if (x == -1) {
    x = (tft.width() - width) / 2;
  }

  bmpFile.seek(imageOffset);
  int rowPadding = (4 - (width * 3) % 4) % 4;
  int rowBytes = width * 3 + rowPadding;

  uint16_t lineBuffer[width];
  uint8_t rowBuffer[rowBytes];

  tft.setSwapBytes(true);
  tft.drawRoundRect(x - 2, y - 2, width + 4, height + 4, 6, TFT_DARKGREY);

  for (int row = 0; row < height; row++) {
    int drawY = flip ? (y + height - 1 - row) : (y + row);
    if (bmpFile.read(rowBuffer, rowBytes) != rowBytes) break;

    int bIdx = 0;
    for (int col = 0; col < width; col++) {
      uint8_t b = rowBuffer[bIdx++];
      uint8_t g = rowBuffer[bIdx++];
      uint8_t r = rowBuffer[bIdx++];
      lineBuffer[col] = tft.color565(r, g, b);
    }

    if (drawY >= 0 && drawY < tft.height()) {
      tft.pushImage(x, drawY, width, 1, lineBuffer);
    }
  }

  tft.setSwapBytes(false);
  bmpFile.close();
  return true;
}

// --- Persistência de Dados (NVS / Flash) ---
void saveCombatantsToNVS() {
  prefs.begin("dnd_data", false);
  String data = "";
  for (size_t i = 0; i < combatants.size(); i++) {
    data += combatants[i].name + "|" + combatants[i].icon + "|" + String(combatants[i].initiative) + "|" + combatants[i].image + "|" + String(combatants[i].active ? 1 : 0) + "\n";
  }
  prefs.putString("list", data);
  prefs.end();
}

void loadCombatantsFromNVS() {
  prefs.begin("dnd_data", true);
  String data = prefs.getString("list", "");
  prefs.end();

  combatants.clear();
  if (data.length() == 0) return;

  int startIdx = 0;
  while (startIdx < data.length()) {
    int endIdx = data.indexOf('\n', startIdx);
    if (endIdx == -1) endIdx = data.length();
    String line = data.substring(startIdx, endIdx);
    startIdx = endIdx + 1;

    int p1 = line.indexOf('|');
    int p2 = line.indexOf('|', p1 + 1);
    int p3 = line.indexOf('|', p2 + 1);
    int p4 = line.indexOf('|', p3 + 1);

    if (p1 != -1 && p2 != -1) {
      Combatant c;
      c.name = line.substring(0, p1);
      c.icon = line.substring(p1 + 1, p2);
      if (p3 != -1) {
        c.initiative = line.substring(p2 + 1, p3).toInt();
        if (p4 != -1) {
          c.image = line.substring(p3 + 1, p4);
          c.active = (line.substring(p4 + 1).toInt() != 0);
        } else {
          c.image = line.substring(p3 + 1);
          c.active = true;
        }
      } else {
        c.initiative = line.substring(p2 + 1).toInt();
        c.image = "";
        c.active = true;
      }
      combatants.push_back(c);
    }
  }
}

// --- Nome Amigável da Classe para a Tela ---
String getClassName(String icon) {
  if (icon == "artificer") return "Artifice";
  if (icon == "barbarian") return "Barbaro";
  if (icon == "bard")      return "Bardo";
  if (icon == "warlock")   return "Bruxo";
  if (icon == "cleric")    return "Clerigo";
  if (icon == "druid")     return "Druida";
  if (icon == "sorcerer")  return "Feiticeiro";
  if (icon == "warrior")   return "Guerreiro";
  if (icon == "rogue")     return "Ladino";
  if (icon == "wizard")    return "Mago";
  if (icon == "monk")      return "Monge";
  if (icon == "paladin")   return "Paladino";
  if (icon == "ranger")    return "Ranger";
  if (icon == "monster")   return "Monstro";
  if (icon == "boss")      return "BOSS";
  return "Heroi";
}

// --- Desenhos dos Ícones de Classe/Monstro no Display (Ampliados para 90x90) ---
void drawClassIcon(int cx, int cy, String icon) {
  tft.drawRoundRect(cx - 45, cy - 45, 90, 90, 10, TFT_WHITE);

  if (icon == "boss") {
    tft.fillRect(cx - 28, cy + 10, 56, 12, TFT_GOLD);
    tft.fillTriangle(cx - 28, cy + 10, cx - 28, cy - 22, cx - 10, cy + 10, TFT_GOLD);
    tft.fillTriangle(cx - 12, cy + 10, cx, cy - 28, cx + 12, cy + 10, TFT_GOLD);
    tft.fillTriangle(cx + 10, cy + 10, cx + 28, cy - 22, cx + 28, cy + 10, TFT_GOLD);
    tft.fillCircle(cx, cy - 28, 4, TFT_RED);
  } else if (icon == "monster") {
    tft.fillCircle(cx, cy - 6, 26, TFT_MAGENTA);
    tft.fillRect(cx - 16, cy + 8, 32, 16, TFT_MAGENTA);
    tft.fillCircle(cx - 10, cy - 8, 6, TFT_BLACK);
    tft.fillCircle(cx + 10, cy - 8, 6, TFT_BLACK);
    tft.fillRect(cx - 10, cy + 16, 20, 4, TFT_WHITE);
  } else if (icon == "warrior") {
    tft.drawLine(cx - 28, cy - 28, cx + 28, cy + 28, TFT_ORANGE);
    tft.drawLine(cx + 28, cy - 28, cx - 28, cy + 28, TFT_ORANGE);
    tft.drawLine(cx - 27, cy - 28, cx + 29, cy + 28, TFT_ORANGE);
    tft.drawLine(cx + 27, cy - 28, cx - 29, cy + 28, TFT_ORANGE);
    tft.fillCircle(cx, cy, 9, TFT_WHITE);
  } else if (icon == "barbarian") {
    tft.fillRect(cx - 4, cy - 30, 8, 60, 0x9A60);
    tft.fillTriangle(cx - 4, cy - 20, cx - 30, cy - 30, cx - 30, cy - 2, TFT_RED);
    tft.fillTriangle(cx + 4, cy - 20, cx + 30, cy - 30, cx + 30, cy - 2, TFT_RED);
  } else if (icon == "paladin") {
    tft.fillTriangle(cx - 24, cy - 22, cx + 24, cy - 22, cx, cy + 28, TFT_GOLD);
    tft.fillRect(cx - 5, cy - 18, 10, 34, TFT_WHITE);
    tft.fillRect(cx - 12, cy - 10, 24, 10, TFT_WHITE);
  } else if (icon == "cleric") {
    tft.fillRect(cx - 6, cy - 30, 12, 60, TFT_GOLD);
    tft.fillRect(cx - 22, cy - 12, 44, 12, TFT_GOLD);
  } else if (icon == "druid") {
    tft.fillCircle(cx, cy - 4, 22, TFT_GREEN);
    tft.fillRect(cx - 5, cy + 12, 10, 18, 0x9A60);
    tft.drawLine(cx - 16, cy - 18, cx, cy + 12, TFT_DARKGREEN);
  } else if (icon == "wizard") {
    tft.fillRect(cx - 24, cy - 20, 48, 40, TFT_CYAN);
    tft.drawLine(cx - 16, cy - 8, cx + 16, cy - 8, TFT_BLACK);
    tft.drawLine(cx - 16, cy, cx + 16, cy, TFT_BLACK);
    tft.drawLine(cx - 16, cy + 8, cx + 8, cy + 8, TFT_BLACK);
  } else if (icon == "sorcerer") {
    tft.fillCircle(cx, cy, 24, TFT_PURPLE);
    tft.drawCircle(cx, cy, 30, TFT_MAGENTA);
    tft.fillCircle(cx - 6, cy - 6, 6, TFT_WHITE);
  } else if (icon == "warlock") {
    tft.fillCircle(cx, cy, 24, TFT_NAVY);
    tft.fillCircle(cx, cy, 12, TFT_VIOLET);
    tft.fillCircle(cx, cy, 4, TFT_BLACK);
  } else if (icon == "rogue") {
    tft.fillTriangle(cx, cy - 30, cx - 8, cy + 8, cx + 8, cy + 8, TFT_SILVER);
    tft.fillRect(cx - 18, cy + 8, 36, 6, TFT_GOLD);
    tft.fillRect(cx - 5, cy + 14, 10, 16, TFT_RED);
  } else if (icon == "ranger") {
    tft.drawCircle(cx - 8, cy, 26, TFT_DARKGREEN);
    tft.drawLine(cx - 8, cy - 26, cx - 8, cy + 26, TFT_WHITE);
    tft.drawLine(cx - 20, cy, cx + 24, cy, TFT_YELLOW);
  } else if (icon == "monk") {
    tft.fillCircle(cx, cy - 4, 19, TFT_ORANGE);
    tft.fillRect(cx - 20, cy + 14, 40, 10, TFT_BLACK);
    tft.fillRect(cx - 5, cy + 24, 10, 12, TFT_BLACK);
  } else if (icon == "bard") {
    tft.fillCircle(cx - 8, cy + 12, 10, TFT_PINK);
    tft.fillRect(cx, cy - 22, 6, 34, TFT_PINK);
    tft.fillRect(cx - 20, cy - 22, 20, 8, TFT_PINK);
  } else if (icon == "artificer") {
    tft.fillCircle(cx, cy, 22, TFT_SILVER);
    tft.fillCircle(cx, cy, 9, TFT_BLACK);
    tft.fillRect(cx - 5, cy - 30, 10, 60, TFT_SILVER);
    tft.fillRect(cx - 30, cy - 5, 60, 10, TFT_SILVER);
  }
}

// --- Telas do Display no Modo Retrato ---
void updateDisplay() {
  tft.fillScreen(TFT_BLACK);
  int w = tft.width();
  int h = tft.height();

  if (isApMode) {
    tft.setTextColor(TFT_ORANGE, TFT_BLACK);
    tft.drawCentreString("CONFIGURACAO", w / 2, 12, 4);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawCentreString("Wi-Fi Nao Encontrado!", w / 2, 42, 2);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Conecte na rede:", w / 2, 75, 2);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawCentreString("DnD-Config", w / 2, 98, 4);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Senha: d20nat20", w / 2, 135, 2);
    tft.drawCentreString("Acesse no Navegador:", w / 2, 175, 2);

    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawCentreString("http://192.168.4.1", w / 2, 200, 2);
    return;
  }

  if (!inCombat) {
    tft.setTextColor(TFT_GOLD, TFT_BLACK);
    tft.drawCentreString("D&D TRACKER", w / 2, 16, 4);

    int cx = w / 2;
    int cy = h / 2 - 20;
    tft.drawCircle(cx, cy, 26, TFT_DARKGREY);
    tft.fillTriangle(cx, cy - 20, cx - 18, cy + 12, cx + 18, cy + 12, TFT_PURPLE);
    tft.setTextColor(TFT_WHITE, TFT_PURPLE);
    tft.drawCentreString("20", cx, cy - 3, 2);

    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawCentreString("Aguardando", w / 2, cy + 40, 2);
    tft.drawCentreString("Combate...", w / 2, cy + 58, 2);

    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    String ipStr = "http://" + WiFi.localIP().toString();
    tft.drawCentreString(ipStr, w / 2, h - 30, 2);
    return;
  }

  Combatant current = combatants[currentIndex];

  int activeCount = 0;
  int activePos = 0;
  for (size_t i = 0; i < combatants.size(); i++) {
    if (combatants[i].active) {
      activeCount++;
      if ((int)i <= currentIndex) activePos = activeCount;
    }
  }

  // Barra de Rodada no Topo
  tft.fillRect(0, 0, w, 24, TFT_NAVY);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString("ROD. " + String(currentRound), 6, 4, 2);
  tft.drawRightString(String(activePos) + "/" + String(activeCount), w - 6, 4, 2);

  // Espaço da Imagem Dobrado (160x160 px centralizado)
  bool imgDrawn = false;
  if (current.image.length() > 0 && sdAvailable) {
    imgDrawn = drawBmp(current.image.c_str(), -1, 26);
  }

  if (!imgDrawn) {
    drawClassIcon(w / 2, 101, current.icon);
  }

  // Nome do Combatente
  uint16_t nameColor = (current.icon == "boss" ? TFT_GOLD : (current.icon == "monster" ? TFT_RED : TFT_GREEN));
  tft.setTextColor(nameColor, TFT_BLACK);
  int nameFont = (w < 160) ? 2 : 4;
  tft.drawCentreString(current.name.substring(0, 12), w / 2, 186, nameFont);

  // Classe e Iniciativa combinados
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  String infoLine = "[" + getClassName(current.icon) + "]  Inic: " + String(current.initiative);
  tft.drawCentreString(infoLine, w / 2, 212, 2);

  tft.drawLine(10, 230, w - 10, 230, TFT_DARKGREY);

  // Próximo da fila
  int nextIndex = currentIndex;
  for (size_t i = 1; i <= combatants.size(); i++) {
    int cand = (currentIndex + i) % combatants.size();
    if (combatants[cand].active) {
      nextIndex = cand;
      break;
    }
  }
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawCentreString("Prox: " + combatants[nextIndex].name + " (" + String(combatants[nextIndex].initiative) + ")", w / 2, 234, 2);

  // Barra de Timer
  if (timerRunning) {
    int rem = max(0, (int)((timerEndMillis - millis()) / 1000));
    int m = rem / 60;
    int s = rem % 60;
    char timeStr[20];
    sprintf(timeStr, "TEMPO: %02d:%02d", m, s);
    uint16_t tColor = (rem <= 10) ? TFT_RED : TFT_YELLOW;

    tft.fillRoundRect(20, 252, w - 40, 24, 4, TFT_NAVY);
    tft.setTextColor(tColor, TFT_NAVY);
    tft.drawCentreString(timeStr, w / 2, 256, 2);
  } else if (timerExpired && (millis() - timerExpiredMillis < 5000)) {
    tft.fillRoundRect(15, 252, w - 30, 24, 4, TFT_RED);
    tft.setTextColor(TFT_WHITE, TFT_RED);
    tft.drawCentreString("TEMPO ESGOTADO!", w / 2, 256, 2);
  }

  // Botões de Toque na Base da Tela
  int btnH = 34;
  int btnY = h - btnH - 6;

  tft.fillRoundRect(4, btnY, (w / 2) - 6, btnH, 5, 0x4208);
  tft.setTextColor(TFT_WHITE, 0x4208);
  tft.drawCentreString("<< VOLTAR", (w / 4) + 1, btnY + 9, 2);

  tft.fillRoundRect((w / 2) + 2, btnY, (w / 2) - 6, btnH, 5, TFT_NAVY);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawCentreString("AVANCAR >>", (w * 3 / 4) - 1, btnY + 9, 2);
}

// --- Interface Web (HTML, CSS e JavaScript) ---
const char INDEX_HTML[] = R"=====(
<!DOCTYPE html>
<html lang="pt-BR">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>D&D Initiative Manager</title>
  <style>
    body { background-color: #121212; color: #e0e0e0; font-family: sans-serif; margin: 0; padding: 12px; }
    h1 { color: #d4af37; text-align: center; margin: 10px 0; font-size: 1.5rem; }
    .card { background: #1e1e1e; border: 1px solid #333; border-radius: 8px; padding: 14px; margin-bottom: 12px; max-width: 480px; margin-left: auto; margin-right: auto; }
    input, select, button { width: 100%; padding: 11px; margin: 5px 0; border-radius: 6px; box-sizing: border-box; font-size: 0.95rem; }
    input, select { background: #2a2a2a; border: 1px solid #444; color: #fff; }
    button { font-weight: bold; cursor: pointer; border: none; }
    .btn-add { background: #2e7d32; color: white; margin-top: 8px; }
    .btn-start { background: #c62828; color: white; font-size: 1.1rem; }
    .btn-prev { background: #e65100; color: white; font-size: 1.1rem; margin-bottom: 6px; }
    .btn-next { background: #1565c0; color: white; font-size: 1.1rem; margin-bottom: 6px; }
    .btn-end { background: #424242; color: white; margin-top: 4px; }
    .btn-sec { background: #37474f; color: #eee; font-size: 0.95rem; margin: 6px 0; display: block; text-align: center; border-radius: 6px; text-decoration: none; padding: 10px; box-sizing: border-box; font-weight: bold; cursor: pointer; }
    .combatant-item { display: flex; justify-content: space-between; align-items: center; padding: 8px 10px; border-bottom: 1px solid #2d2d2d; }
    .item-actions { display: flex; align-items: center; gap: 4px; }
    .item-actions button { width: auto; padding: 5px 8px; margin: 0; font-size: 0.85rem; background: #333; color: #fff; border: 1px solid #555; }
    .status-badge { font-weight: bold; color: #d4af37; text-align: center; font-size: 1.1rem; margin-bottom: 10px; }
    .collapsible { background: #292929; color: #aaa; padding: 8px; border-radius: 5px; cursor: pointer; text-align: center; font-size: 0.9rem; margin-top: 10px; border: 1px dashed #444; }
    .init-input { width: 56px !important; padding: 5px !important; text-align: center; font-weight: bold; margin: 0 4px 0 0 !important; background: #111 !important; color: #ffd700 !important; border: 1px solid #555 !important; border-radius: 4px; }
    .chk-active { width: 22px !important; height: 22px !important; margin: 0 8px 0 0 !important; cursor: pointer; accent-color: #2e7d32; }
    .timer-btn { flex: 1; margin: 0 2px; padding: 9px 4px; font-size: 0.85rem; }
  </style>
</head>
<body>
  <h1>⚔️ D&D INITIATIVE ⚔️</h1>
  
  <!-- CONTROLE DE COMBATE -->
  <div class="card">
    <div id="status" class="status-badge">Aguardando combate...</div>
    <div id="combatControls">
      <button class="btn-start" onclick="action('/start')">Iniciar Combate ⚔️</button>
    </div>
  </div>

  <!-- TIMER DE TURNO -->
  <div class="card">
    <h3 style="margin: 0 0 6px 0;">⏱️ Timer de Turno</h3>
    <div id="timerDisplay" style="text-align: center; font-size: 1.3rem; font-weight: bold; color: #ffeb3b; margin: 4px 0;">Timer Parado</div>
    
    <div style="display: flex; gap: 4px; margin-top: 6px;">
      <button class="btn-sec timer-btn" style="background: #0277bd;" onclick="startTimer(30)">30s</button>
      <button class="btn-sec timer-btn" style="background: #0277bd;" onclick="startTimer(60)">1m</button>
      <button class="btn-sec timer-btn" style="background: #0277bd;" onclick="startTimer(120)">2m</button>
      <button class="btn-sec timer-btn" style="background: #0277bd;" onclick="startTimer(300)">5m</button>
      <button class="btn-sec timer-btn" style="background: #c62828;" onclick="stopTimer()">🛑 Parar</button>
    </div>

    <div style="display: flex; gap: 6px; align-items: center; margin-top: 8px;">
      <input type="number" id="customTimerMin" placeholder="Min" min="0" max="99" style="flex: 1; margin: 0; padding: 8px; text-align: center;">
      <span style="font-weight: bold;">:</span>
      <input type="number" id="customTimerSec" placeholder="Seg" min="0" max="59" style="flex: 1; margin: 0; padding: 8px; text-align: center;">
      <button class="btn-sec" style="flex: 2; background: #2e7d32; margin: 0; padding: 8px;" onclick="startCustomTimer()">▶ Iniciar</button>
    </div>
  </div>

  <!-- GERENCIADOR DE ENCONTROS NO CARTÃO SD -->
  <div class="card">
    <h3 style="margin: 0 0 8px 0;">🗺️ Encontros no Cartão SD</h3>
    <div id="sdStatusText" style="color: #aaa; font-size: 0.85rem; margin-bottom: 6px;">Verificando cartão...</div>
    
    <!-- Salvar como novo -->
    <div style="display: flex; gap: 6px; margin-bottom: 12px;">
      <input type="text" id="newEncounterName" placeholder="Novo encontro (ex: covil_dragao)" style="flex: 2; margin: 0;">
      <button class="btn-sec" style="flex: 1; margin: 0; background: #0277bd;" onclick="saveNewEncounter()">➕ Criar Novo</button>
    </div>

    <!-- Lista de Encontros com Opções de Sobrescrever e Deletar -->
    <div id="encountersList" style="margin-top: 6px;"></div>
  </div>

  <!-- UPLOAD DE IMAGEM PARA O CARTÃO SD -->
  <div class="card">
    <h3 style="margin: 0 0 8px 0;">🖼️ Enviar Imagem para o Cartão SD</h3>
    <div style="color: #aaa; font-size: 0.85rem; margin-bottom: 6px;">Escolha o herói e selecione a foto (PNG, JPG, BMP). Ela será otimizada para 160x160 px:</div>
    
    <label style="font-size: 0.85rem; color: #d4af37;">1. Qual combatente vai receber a foto?</label>
    <select id="uploadTargetHero">
      <option value="-1">-- Selecione o Herói/Monstro --</option>
    </select>

    <label style="font-size: 0.85rem; color: #d4af37; margin-top: 4px; display: block;">2. Selecione a imagem:</label>
    <input type="file" id="imgUploadInput" accept=".png,.jpg,.jpeg,.bmp,.webp,image/*">
    
    <button class="btn-sec" style="background: #6a1b9a;" onclick="uploadForSelectedHero()">📤 Enviar & Associar ao Combatente</button>
    <div id="uploadStatusText" style="color: #00e676; font-size: 0.85rem; text-align: center; margin-top: 4px;"></div>
  </div>

  <!-- ADICIONAR COMBATENTE -->
  <div class="card">
    <h3 style="margin: 0 0 8px 0;">Adicionar Combatente</h3>
    <input type="text" id="name" placeholder="Nome do Personagem ou Monstro">
    <select id="icon">
      <option value="warrior" selected>⚔️ Guerreiro</option>
      <option value="barbarian">🪓 Bárbaro</option>
      <option value="paladin">🛡️ Paladino</option>
      <option value="cleric">✝️ Clérigo</option>
      <option value="druid">🌿 Druida</option>
      <option value="wizard">📜 Mago</option>
      <option value="sorcerer">🔮 Feiticeiro</option>
      <option value="warlock">👁️‍🗨️ Bruxo</option>
      <option value="rogue">🗡️ Ladino</option>
      <option value="ranger">🏹 Ranger (Patrulheiro)</option>
      <option value="monk">🥋 Monge</option>
      <option value="bard">🪕 Bardo</option>
      <option value="artificer">🛠️ Artífice</option>
      <option value="monster">👾 Monstro</option>
      <option value="boss">👑 Boss</option>
    </select>
    <input type="number" id="init" placeholder="Iniciativa (ex: 18)">
    <input type="text" id="img" list="sdImagesDatalist" placeholder="Arquivo no SD (opcional, ex: conan.bmp)">
    <datalist id="sdImagesDatalist"></datalist>
    <button class="btn-add" onclick="addCombatant()">Adicionar à Fila</button>
  </div>

  <!-- LISTA DE INICIATIVA E EDIÇÃO -->
  <div class="card">
    <h3 style="margin: 0 0 8px 0;">Lista de Iniciativa</h3>
    <div id="list"></div>
    <button style="background: transparent; color: #777; border: 1px solid #444; margin-top: 10px;" onclick="action('/clear')">Limpar Todos</button>
  </div>

  <!-- GERENCIAR JSON (EXPORTAR / IMPORTAR / COLAR) -->
  <div class="card">
    <h3 style="margin: 0 0 8px 0;">📂 Salvar & Carregar (JSON)</h3>
    <button class="btn-sec" onclick="exportJson()">💾 Baixar Lista Atual (Exportar JSON)</button>
    <label class="btn-sec" style="background: #2e7d32;">
      📥 Carregar Arquivo JSON (Do Celular)
      <input type="file" id="jsonFileInput" accept="*/*" style="display:none;" onchange="importJson(this)">
    </label>
    <button class="btn-sec" style="background: #455a64;" onclick="pasteJson()">📋 Colar Texto JSON (Alternativa)</button>
  </div>

  <!-- CONFIGURAÇÃO DE WI-FI -->
  <div class="card">
    <div class="collapsible" onclick="toggleWifiSection()">⚙️ Alterar Rede Wi-Fi (Clique para expandir)</div>
    <div id="wifiSection" style="display:none; margin-top: 10px;">
      <input type="text" id="wifi_ssid" placeholder="Nome da Rede Wi-Fi (SSID)">
      <input type="password" id="wifi_pass" placeholder="Senha do Wi-Fi">
      <button style="background: #d84315; color: white;" onclick="saveWifiConfig()">Salvar Wi-Fi & Reiniciar</button>
    </div>
  </div>

  <script>
    let labels = {
      artificer: '🛠️ Artífice', barbarian: '🪓 Bárbaro', bard: '🪕 Bardo',
      warlock: '👁️‍🗨️ Bruxo', cleric: '✝️ Clérigo', druid: '🌿 Druida',
      sorcerer: '🔮 Feiticeiro', warrior: '⚔️ Guerreiro', rogue: '🗡️ Ladino',
      wizard: '📜 Mago', monk: '🥋 Monge', paladin: '🛡️ Paladino',
      ranger: '🏹 Ranger', monster: '👾 Monstro', boss: '👑 Boss'
    };

    let currentList = [];
    let quickUploadHeroIdx = -1;

    function loadStatus() {
      fetch('/status')
        .then(r => r.json())
        .then(data => {
          currentList = data.list;

          // Atualiza Timer
          let tDisp = document.getElementById('timerDisplay');
          if (data.timerRunning) {
            let m = Math.floor(data.timerSec / 60);
            let s = data.timerSec % 60;
            tDisp.innerText = `⏳ ${(m<10?'0':'')+m}:${(s<10?'0':'')+s}`;
            tDisp.style.color = (data.timerSec <= 10) ? '#ff5252' : '#ffeb3b';
          } else if (data.timerExpired) {
            tDisp.innerText = "⚠️ TEMPO ESGOTADO! ⚠️";
            tDisp.style.color = "#ff5252";
          } else {
            tDisp.innerText = "Timer Parado";
            tDisp.style.color = "#888";
          }

          // Atualiza Controles de Combate
          let controls = '';
          if (data.inCombat) {
            document.getElementById('status').innerText = `Rodada: ${data.round} | Vez de: ${data.current}`;
            controls = `
              <button class="btn-next" onclick="action('/next')">Próximo Turno ⏩</button>
              <button class="btn-prev" onclick="action('/prev')">⏪ Turno Anterior</button>
              <button class="btn-end" onclick="action('/end')">Finalizar Combate 🛑</button>
            `;
          } else {
            document.getElementById('status').innerText = "Fora de combate";
            controls = `<button class="btn-start" onclick="action('/start')">Iniciar Combate ⚔️</button>`;
          }
          document.getElementById('combatControls').innerHTML = controls;

          // Atualiza Dropdown do formulário de Upload
          let uploadSel = document.getElementById('uploadTargetHero');
          let prevSelected = uploadSel.value;
          let optHtml = '<option value="-1">-- Selecione o Herói/Monstro --</option>';
          data.list.forEach((c, idx) => {
            optHtml += `<option value="${idx}">${c.name} (${labels[c.icon] || '👤'})</option>`;
          });
          uploadSel.innerHTML = optHtml;
          if (prevSelected !== "-1") uploadSel.value = prevSelected;

          // Renderiza Lista de Combatentes
          let listHtml = '';
          data.list.forEach((c, idx) => {
            let isCurrent = (data.inCombat && idx === data.currentIndex) ? 'style="color:#00e676; font-weight:bold; background:#223322;"' : '';
            let opacity = c.active ? '1.0' : '0.45';
            let strike = c.active ? '' : 'text-decoration: line-through;';
            let imgBadge = c.image ? ' 🖼️' : '';

            listHtml += `<div class="combatant-item" ${isCurrent} style="opacity: ${opacity};">
              <div style="display: flex; align-items: center;">
                <input type="checkbox" class="chk-active" ${c.active ? 'checked' : ''} onchange="toggleActive(${idx})" title="Ativar/Desativar combatente">
                <input type="number" class="init-input" value="${c.initiative}" onchange="quickUpdateInit(${idx}, this.value)" title="Alterar iniciativa rapidamente">
                <span style="${strike}">${labels[c.icon] || '👤'} ${c.name}${imgBadge}</span>
              </div>
              <div class="item-actions">
                <button onclick="triggerHeroCamera(${idx})" title="Adicionar ou trocar foto">📷</button>
                <button onclick="editItem(${idx}, '${encodeURIComponent(c.name)}', ${c.initiative}, '${c.icon}', '${encodeURIComponent(c.image || '')}')">✏️</button>
                <button onclick="deleteItem(${idx})">❌</button>
              </div>
            </div>`;
          });
          document.getElementById('list').innerHTML = listHtml || '<p style="color:#666">Nenhum combatente cadastrado.</p>';
        })
        .catch(e => console.log('Aguardando servidor...'));
    }

    // Timer
    function startTimer(seconds) {
      fetch('/timer?action=start&sec=' + seconds).then(() => loadStatus());
    }

    function startCustomTimer() {
      let m = parseInt(document.getElementById('customTimerMin').value) || 0;
      let s = parseInt(document.getElementById('customTimerSec').value) || 0;
      let total = (m * 60) + s;
      if (total <= 0) return alert('Informe um tempo maior que 0 segundos!');
      startTimer(total);
      document.getElementById('customTimerMin').value = '';
      document.getElementById('customTimerSec').value = '';
    }

    function stopTimer() {
      fetch('/timer?action=stop').then(() => loadStatus());
    }

    // Ativar / Desativar
    function toggleActive(idx) {
      fetch('/toggle_active?idx=' + idx).then(() => loadStatus());
    }

    // Edição Rápida de Iniciativa
    function quickUpdateInit(idx, val) {
      let init = parseInt(val);
      if (isNaN(init)) return;
      fetch(`/quick_init?idx=${idx}&init=${init}`).then(() => loadStatus());
    }

    // Gerenciador de Encontros no Cartão SD
    function checkSdEncounters() {
      fetch('/list_encounters')
        .then(r => r.json())
        .then(files => {
          let container = document.getElementById('encountersList');
          let status = document.getElementById('sdStatusText');
          if (!files || files.length === 0) {
            container.innerHTML = '<p style="color:#777; font-size:0.85rem; margin:6px 0;">Nenhum encontro salvo no cartão SD.</p>';
            status.innerText = "Cartão SD conectado (pasta /encounters vazia).";
          } else {
            status.innerText = `Encontrado(s) ${files.length} encontro(s) no cartão SD:`;
            let html = '';
            files.forEach(f => {
              let displayName = f.replace('.json', '').replace(/_/g, ' ');
              html += `
                <div style="display:flex; justify-content:space-between; align-items:center; padding:7px 0; border-bottom:1px solid #333;">
                  <span style="font-size:0.9rem; font-weight:bold; color:#e0e0e0;">📄 ${displayName}</span>
                  <div style="display:flex; gap:4px;">
                    <button class="btn-sec" style="margin:0; padding:5px 8px; font-size:0.8rem; background:#1b5e20;" onclick="loadEncounterFile('${f}')" title="Carregar na mesa">📥 Carregar</button>
                    <button class="btn-sec" style="margin:0; padding:5px 8px; font-size:0.8rem; background:#0277bd;" onclick="overwriteEncounterFile('${f}')" title="Substituir com a lista atual">💾 Salvar</button>
                    <button class="btn-sec" style="margin:0; padding:5px 8px; font-size:0.8rem; background:#c62828;" onclick="deleteEncounterFile('${f}')" title="Excluir do cartão SD">🗑️</button>
                  </div>
                </div>
              `;
            });
            container.innerHTML = html;
          }
        })
        .catch(() => {
          document.getElementById('sdStatusText').innerText = "Cartão SD não detectado.";
        });
    }

    function loadEncounterFile(file) {
      if (confirm(`Carregar o encontro "${file}"? A lista atual da mesa será substituída.`)) {
        fetch('/get_encounter?file=' + encodeURIComponent(file))
          .then(r => r.json())
          .then(list => {
            processImportedList(list);
          })
          .catch(err => alert('Erro ao carregar do SD: ' + err.message));
      }
    }

    function overwriteEncounterFile(file) {
      if (currentList.length === 0) return alert('A lista atual está vazia!');
      if (confirm(`Deseja SOBRESCREVER o arquivo "${file}" com os combatentes atuais da mesa?`)) {
        saveEncounterFile(file);
      }
    }

    function saveNewEncounter() {
      if (currentList.length === 0) return alert('A lista atual está vazia!');
      let nameInput = document.getElementById('newEncounterName');
      let name = nameInput.value.trim();
      if (!name) return alert('Digite o nome do novo encontro!');

      let filename = name.replace(/[^a-zA-Z0-9_\-]/g, '_').toLowerCase();
      if (!filename.endsWith('.json')) filename += '.json';

      saveEncounterFile(filename);
      nameInput.value = '';
    }

    function saveEncounterFile(filename) {
      fetch('/save_encounter?file=' + encodeURIComponent(filename), {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(currentList, null, 2)
      })
      .then(r => r.text())
      .then(msg => {
        alert(msg);
        checkSdEncounters();
      })
      .catch(err => alert('Erro ao salvar no SD: ' + err.message));
    }

    function deleteEncounterFile(file) {
      if (confirm(`Tem certeza que deseja EXCLUIR definitivamente o arquivo "${file}" do cartão SD?`)) {
        fetch('/delete_encounter?file=' + encodeURIComponent(file))
          .then(r => r.text())
          .then(msg => {
            alert(msg);
            checkSdEncounters();
          })
          .catch(err => alert('Erro ao excluir: ' + err.message));
      }
    }

    function checkSdImages() {
      fetch('/list_images')
        .then(r => r.json())
        .then(images => {
          let datalist = document.getElementById('sdImagesDatalist');
          let options = '';
          images.forEach(img => {
            options += `<option value="${img}"></option>`;
          });
          datalist.innerHTML = options;
        })
        .catch(e => console.log(e));
    }

    // Conversor de Imagem para BMP 24-bit com assinatura exata
    function canvasToBmpBlob(canvas) {
      let width = canvas.width;
      let height = canvas.height;
      let ctx = canvas.getContext('2d');
      let imgData = ctx.getImageData(0, 0, width, height);
      let data = imgData.data;

      let rowPadding = (4 - (width * 3) % 4) % 4;
      let rowSize = width * 3 + rowPadding;
      let pixelArraySize = rowSize * height;
      let fileSize = 54 + pixelArraySize;

      let buffer = new ArrayBuffer(fileSize);
      let view = new DataView(buffer);

      view.setUint8(0, 0x42);
      view.setUint8(1, 0x4D);
      view.setUint32(2, fileSize, true);
      view.setUint32(6, 0, true);
      view.setUint32(10, 54, true);

      view.setUint32(14, 40, true);
      view.setInt32(18, width, true);
      view.setInt32(22, height, true);
      view.setUint16(26, 1, true);
      view.setUint16(28, 24, true);
      view.setUint32(30, 0, true);
      view.setUint32(34, pixelArraySize, true);
      view.setInt32(38, 2835, true);
      view.setInt32(42, 2835, true);
      view.setUint32(46, 0, true);
      view.setUint32(50, 0, true);

      let offset = 54;
      for (let y = height - 1; y >= 0; y--) {
        for (let x = 0; x < width; x++) {
          let idx = (y * width + x) * 4;
          let r = data[idx];
          let g = data[idx + 1];
          let b = data[idx + 2];
          let a = data[idx + 3] / 255;
          view.setUint8(offset++, Math.round(b * a));
          view.setUint8(offset++, Math.round(g * a));
          view.setUint8(offset++, Math.round(r * a));
        }
        for (let p = 0; p < rowPadding; p++) {
          view.setUint8(offset++, 0);
        }
      }
      return new Blob([buffer], { type: 'image/bmp' });
    }

    function sanitizeFilename(name) {
      let clean = name.normalize("NFD").replace(/[\u0300-\u036f]/g, "");
      clean = clean.toLowerCase().trim().replace(/[^a-z0-9_\-]/g, "_");
      clean = clean.replace(/_+/g, "_");
      if (!clean) clean = "heroi";
      return clean + ".bmp";
    }

    function triggerHeroCamera(idx) {
      quickUploadHeroIdx = idx;
      document.getElementById('uploadTargetHero').value = idx;
      document.getElementById('imgUploadInput').click();
    }

    function uploadForSelectedHero() {
      let heroIdx = quickUploadHeroIdx !== -1 ? quickUploadHeroIdx : parseInt(document.getElementById('uploadTargetHero').value);
      if (isNaN(heroIdx) || heroIdx < 0 || heroIdx >= currentList.length) {
        return alert('Por favor, selecione qual combatente vai receber esta imagem!');
      }

      let hero = currentList[heroIdx];
      let input = document.getElementById('imgUploadInput');
      let file = input.files[0];
      if (!file) return alert('Selecione uma imagem primeiro!');

      let status = document.getElementById('uploadStatusText');
      status.innerText = `Processando foto para ${hero.name}...`;

      let targetFilename = sanitizeFilename(hero.name);

      let reader = new FileReader();
      reader.onload = function(e) {
        let img = new Image();
        img.onload = function() {
          let targetSize = 160;
          let canvas = document.createElement('canvas');
          canvas.width = targetSize;
          canvas.height = targetSize;
          let ctx = canvas.getContext('2d');

          ctx.fillStyle = '#000000';
          ctx.fillRect(0, 0, targetSize, targetSize);

          let scale = Math.min(targetSize / img.width, targetSize / img.height);
          let w = img.width * scale;
          let h = img.height * scale;
          let x = (targetSize - w) / 2;
          let y = (targetSize - h) / 2;
          ctx.drawImage(img, x, y, w, h);

          let bmpBlob = canvasToBmpBlob(canvas);
          let formData = new FormData();
          formData.append('data', bmpBlob, targetFilename);

          status.innerText = `Enviando "${targetFilename}" para o SD...`;

          fetch('/upload_image', {
            method: 'POST',
            body: formData
          })
          .then(r => r.text())
          .then(msg => {
            return fetch(`/edit?idx=${heroIdx}&name=${encodeURIComponent(hero.name)}&icon=${hero.icon}&init=${hero.initiative}&img=${encodeURIComponent(targetFilename)}`);
          })
          .then(() => {
            alert(`Sucesso!\nImagem salva como "${targetFilename}" e já associada a ${hero.name}!`);
            status.innerText = `Imagem ativa: ${targetFilename}`;
            input.value = '';
            quickUploadHeroIdx = -1;
            checkSdImages();
            loadStatus();
          })
          .catch(err => {
            alert('Erro no envio da imagem: ' + err.message);
            status.innerText = "Falha no envio.";
          });
        };
        img.src = e.target.result;
      };
      reader.readAsDataURL(file);
    }

    document.addEventListener("DOMContentLoaded", () => {
      document.getElementById('imgUploadInput').addEventListener('change', () => {
        if (quickUploadHeroIdx !== -1) {
          uploadForSelectedHero();
        }
      });
    });

    function action(endpoint) {
      fetch(endpoint).then(() => loadStatus());
    }

    function addCombatant() {
      let name = document.getElementById('name').value.trim();
      let icon = document.getElementById('icon').value;
      let init = document.getElementById('init').value;
      let img  = document.getElementById('img').value.trim();
      if (!name || !init) return alert('Preencha o nome e a iniciativa!');

      fetch(`/add?name=${encodeURIComponent(name)}&icon=${icon}&init=${init}&img=${encodeURIComponent(img)}`)
        .then(() => {
          document.getElementById('name').value = '';
          document.getElementById('init').value = '';
          document.getElementById('img').value = '';
          loadStatus();
        });
    }

    function deleteItem(idx) {
      if (confirm('Remover este combatente?')) {
        fetch('/delete?idx=' + idx).then(() => loadStatus());
      }
    }

    function editItem(idx, encodedName, currentInit, currentIcon, encodedImg) {
      let currentName = decodeURIComponent(encodedName);
      let currentImg = decodeURIComponent(encodedImg || '');
      let name = prompt("Nome:", currentName);
      if (name === null) return;
      let init = prompt("Iniciativa:", currentInit);
      if (init === null) return;
      let img = prompt("Arquivo no SD (ex: conan.bmp):", currentImg);
      if (img === null) img = currentImg;

      fetch(`/edit?idx=${idx}&name=${encodeURIComponent(name)}&icon=${currentIcon}&init=${init}&img=${encodeURIComponent(img)}`).then(() => loadStatus());
    }

    function exportJson() {
      let dataStr = "data:text/json;charset=utf-8," + encodeURIComponent(JSON.stringify(currentList, null, 2));
      let a = document.createElement('a');
      a.setAttribute("href", dataStr);
      a.setAttribute("download", "dnd_iniciativa.json");
      a.click();
    }

    async function processImportedList(list) {
      if (!Array.isArray(list) || list.length === 0) {
        return alert('JSON inválido ou vazio! Deve ser uma lista de combatentes.');
      }
      try {
        await fetch('/clear');
        for (let c of list) {
          let name = encodeURIComponent(c.name || 'Heroi');
          let icon = c.icon || 'warrior';
          let init = c.initiative || 10;
          let img  = encodeURIComponent(c.image || '');
          let act  = (c.active !== false) ? '1' : '0';
          await fetch(`/add?name=${name}&icon=${icon}&init=${init}&img=${img}&active=${act}`);
        }
        loadStatus();
        alert('Personagens carregados com sucesso!');
      } catch (err) {
        alert('Erro ao enviar para o ESP32: ' + err.message);
      }
    }

    function importJson(input) {
      let file = input.files[0];
      if (!file) return;
      let reader = new FileReader();
      reader.onload = function(e) {
        try {
          let list = JSON.parse(e.target.result);
          processImportedList(list);
        } catch(err) {
          alert('Erro ao ler o arquivo JSON: ' + err.message);
        }
      };
      reader.readAsText(file);
      input.value = '';
    }

    function pasteJson() {
      let text = prompt("Cole o conteúdo do seu JSON aqui:");
      if (!text) return;
      try {
        let list = JSON.parse(text);
        processImportedList(list);
      } catch(err) {
        alert("Texto JSON inválido: " + err.message);
      }
    }

    function toggleWifiSection() {
      let el = document.getElementById('wifiSection');
      el.style.display = el.style.display === 'none' ? 'block' : 'none';
    }

    function saveWifiConfig() {
      let s = document.getElementById('wifi_ssid').value.trim();
      let p = document.getElementById('wifi_pass').value;
      if (!s) return alert('Digite o nome da rede (SSID)!');
      if (confirm(`O ESP32 vai reiniciar para conectar em "${s}". Deseja continuar?`)) {
        fetch(`/set_wifi?ssid=${encodeURIComponent(s)}&pass=${encodeURIComponent(p)}`)
          .then(() => alert('Configuração salva! O ESP32 está reiniciando...'));
      }
    }

    setInterval(loadStatus, 2000);
    loadStatus();
    checkSdEncounters();
    checkSdImages();
  </script>
</body>
</html>
)=====";

void sendCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "*");
  server.sendHeader("Access-Control-Allow-Private-Network", "true");
}

// --- Callback de Upload de Imagem para o MicroSD ---
void handleFileUpload() {
  if (!sdAvailable) return;
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    int slash = filename.lastIndexOf('/');
    if (slash != -1) filename = filename.substring(slash + 1);
    int backslash = filename.lastIndexOf('\\');
    if (backslash != -1) filename = filename.substring(backslash + 1);

    String path = "/images/" + filename;
    Serial.printf("[SD UPLOAD] Gravando: %s\n", path.c_str());

    if (SD.exists(path)) {
      SD.remove(path);
    }
    uploadFile = SD.open(path, FILE_WRITE);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
      Serial.printf("[SD UPLOAD] Concluido: %u bytes\n", upload.totalSize);
    }
  }
}

// --- Rotas do Web Server ---
void handleRoot() {
  sendCorsHeaders();
  server.send(200, "text/html", INDEX_HTML);
}

void handleFavicon() {
  sendCorsHeaders();
  server.send(204, "text/plain", "");
}

void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) {
    sendCorsHeaders();
    server.send(204, "text/plain", "");
    return;
  }
  if (server.uri() == "/favicon.ico") {
    handleFavicon();
    return;
  }
  sendCorsHeaders();
  server.send(404, "text/plain", "Nao encontrado");
}

void handleAdd() {
  if (server.hasArg("name") && server.hasArg("init") && server.hasArg("icon")) {
    Combatant c;
    c.name = server.arg("name");
    c.icon = server.arg("icon");
    c.initiative = server.arg("init").toInt();
    c.image = server.hasArg("img") ? server.arg("img") : "";
    c.active = server.hasArg("active") ? (server.arg("active") == "1") : true;
    combatants.push_back(c);
    saveCombatantsToNVS();
  }
  sendCorsHeaders();
  server.send(200, "text/plain", "OK");
}

void handleDelete() {
  if (server.hasArg("idx")) {
    int idx = server.arg("idx").toInt();
    if (idx >= 0 && idx < (int)combatants.size()) {
      combatants.erase(combatants.begin() + idx);
      if (currentIndex >= (int)combatants.size()) currentIndex = 0;
      saveCombatantsToNVS();
      updateDisplay();
    }
  }
  sendCorsHeaders();
  server.send(200, "text/plain", "OK");
}

void handleEdit() {
  if (server.hasArg("idx") && server.hasArg("name") && server.hasArg("init") && server.hasArg("icon")) {
    int idx = server.arg("idx").toInt();
    if (idx >= 0 && idx < (int)combatants.size()) {
      combatants[idx].name = server.arg("name");
      combatants[idx].icon = server.arg("icon");
      combatants[idx].initiative = server.arg("init").toInt();
      if (server.hasArg("img")) {
        combatants[idx].image = server.arg("img");
      }
      saveCombatantsToNVS();
      updateDisplay();
    }
  }
  sendCorsHeaders();
  server.send(200, "text/plain", "OK");
}

void handleQuickInit() {
  if (server.hasArg("idx") && server.hasArg("init")) {
    int idx = server.arg("idx").toInt();
    int init = server.arg("init").toInt();
    if (idx >= 0 && idx < (int)combatants.size()) {
      combatants[idx].initiative = init;
      saveCombatantsToNVS();
      updateDisplay();
    }
  }
  sendCorsHeaders();
  server.send(200, "text/plain", "OK");
}

void handleToggleActive() {
  if (server.hasArg("idx")) {
    int idx = server.arg("idx").toInt();
    if (idx >= 0 && idx < (int)combatants.size()) {
      combatants[idx].active = !combatants[idx].active;
      saveCombatantsToNVS();
      if (inCombat && !combatants[currentIndex].active) {
        handleNext();
      } else {
        updateDisplay();
      }
    }
  }
  sendCorsHeaders();
  server.send(200, "text/plain", "OK");
}

void handleTimer() {
  if (server.hasArg("action")) {
    String act = server.arg("action");
    if (act == "start" && server.hasArg("sec")) {
      int sec = server.arg("sec").toInt();
      if (sec > 0) {
        timerRunning = true;
        timerExpired = false;
        timerEndMillis = millis() + (sec * 1000UL);
        lastTimerSecondUpdate = millis();
        updateDisplay();
      }
    } else if (act == "stop") {
      timerRunning = false;
      timerExpired = false;
      updateDisplay();
    }
  }
  sendCorsHeaders();
  server.send(200, "text/plain", "OK");
}

void handleStart() {
  int activeCount = 0;
  for (const auto& c : combatants) {
    if (c.active) activeCount++;
  }
  if (activeCount == 0) {
    sendCorsHeaders();
    server.send(400, "text/plain", "Nenhum combatente ativo");
    return;
  }

  std::sort(combatants.begin(), combatants.end(), [](const Combatant &a, const Combatant &b) {
    return a.initiative > b.initiative;
  });

  currentIndex = 0;
  while (!combatants[currentIndex].active) {
    currentIndex = (currentIndex + 1) % combatants.size();
  }

  currentRound = 1;
  inCombat = true;
  updateDisplay();
  sendCorsHeaders();
  server.send(200, "text/plain", "Iniciado");
}

void handleNext() {
  if (!inCombat || combatants.empty()) {
    sendCorsHeaders();
    server.send(400, "text/plain", "Nao esta em combate");
    return;
  }

  int start = currentIndex;
  do {
    currentIndex++;
    if (currentIndex >= (int)combatants.size()) {
      currentIndex = 0;
      currentRound++;
    }
  } while (!combatants[currentIndex].active && currentIndex != start);

  updateDisplay();
  sendCorsHeaders();
  server.send(200, "text/plain", "Proximo");
}

void handlePrev() {
  if (!inCombat || combatants.empty()) {
    sendCorsHeaders();
    server.send(400, "text/plain", "Nao esta em combate");
    return;
  }

  int start = currentIndex;
  do {
    if (currentIndex > 0) {
      currentIndex--;
    } else {
      if (currentRound > 1) {
        currentRound--;
        currentIndex = combatants.size() - 1;
      } else {
        currentIndex = 0;
        break;
      }
    }
  } while (!combatants[currentIndex].active && currentIndex != start);

  updateDisplay();
  sendCorsHeaders();
  server.send(200, "text/plain", "Anterior");
}

void handleEnd() {
  inCombat = false;
  currentIndex = 0;
  currentRound = 1;
  timerRunning = false;
  timerExpired = false;
  updateDisplay();
  sendCorsHeaders();
  server.send(200, "text/plain", "Finalizado");
}

void handleClear() {
  combatants.clear();
  inCombat = false;
  currentIndex = 0;
  currentRound = 1;
  timerRunning = false;
  timerExpired = false;
  saveCombatantsToNVS();
  updateDisplay();
  sendCorsHeaders();
  server.send(200, "text/plain", "Limpo");
}

void handleStatus() {
  String json = "{";
  json += "\"inCombat\":" + String(inCombat ? "true" : "false") + ",";
  json += "\"round\":" + String(currentRound) + ",";
  json += "\"currentIndex\":" + String(currentIndex) + ",";
  json += "\"current\":\"" + (inCombat && !combatants.empty() ? combatants[currentIndex].name : "") + "\",";

  int rem = 0;
  if (timerRunning && millis() < timerEndMillis) {
    rem = (timerEndMillis - millis()) / 1000;
  }
  json += "\"timerRunning\":" + String(timerRunning ? "true" : "false") + ",";
  json += "\"timerSec\":" + String(rem) + ",";
  json += "\"timerExpired\":" + String(timerExpired ? "true" : "false") + ",";

  json += "\"list\":[";
  for (size_t i = 0; i < combatants.size(); i++) {
    json += "{\"name\":\"" + combatants[i].name + "\",";
    json += "\"icon\":\"" + combatants[i].icon + "\",";
    json += "\"initiative\":" + String(combatants[i].initiative) + ",";
    json += "\"image\":\"" + combatants[i].image + "\",";
    json += "\"active\":" + String(combatants[i].active ? "true" : "false") + "}";
    if (i < combatants.size() - 1) json += ",";
  }
  json += "]}";
  sendCorsHeaders();
  server.send(200, "application/json", json);
}

void handleSetWifi() {
  if (server.hasArg("ssid")) {
    String newSsid = server.arg("ssid");
    String newPass = server.hasArg("pass") ? server.arg("pass") : "";

    prefs.begin("dnd_wifi", false);
    prefs.putString("ssid", newSsid);
    prefs.putString("pass", newPass);
    prefs.end();

    sendCorsHeaders();
    server.send(200, "text/plain", "OK. Reiniciando...");
    delay(1000);
    ESP.restart();
  } else {
    sendCorsHeaders();
    server.send(400, "text/plain", "SSID obrigatorio");
  }
}

// --- Rotas de Encontros e Imagens no Cartão MicroSD ---
void handleListEncounters() {
  String json = "[";
  if (sdAvailable) {
    File dir = SD.open("/encounters");
    if (!dir) dir = SD.open("/");
    if (dir && dir.isDirectory()) {
      File file = dir.openNextFile();
      bool first = true;
      while (file) {
        String fname = file.name();
        if (!file.isDirectory() && fname.endsWith(".json")) {
          int slash = fname.lastIndexOf('/');
          if (slash != -1) fname = fname.substring(slash + 1);
          if (!first) json += ",";
          json += "\"" + fname + "\"";
          first = false;
        }
        file = dir.openNextFile();
      }
    }
  }
  json += "]";
  sendCorsHeaders();
  server.send(200, "application/json", json);
}

void handleListImages() {
  String json = "[";
  if (sdAvailable) {
    File dir = SD.open("/images");
    if (!dir) dir = SD.open("/");
    if (dir && dir.isDirectory()) {
      File file = dir.openNextFile();
      bool first = true;
      while (file) {
        String fname = file.name();
        if (!file.isDirectory() && (fname.endsWith(".bmp") || fname.endsWith(".BMP"))) {
          int slash = fname.lastIndexOf('/');
          if (slash != -1) fname = fname.substring(slash + 1);
          if (!first) json += ",";
          json += "\"" + fname + "\"";
          first = false;
        }
        file = dir.openNextFile();
      }
    }
  }
  json += "]";
  sendCorsHeaders();
  server.send(200, "application/json", json);
}

void handleGetEncounter() {
  if (!sdAvailable || !server.hasArg("file")) {
    sendCorsHeaders();
    server.send(400, "text/plain", "Parametro invalido");
    return;
  }
  String filename = server.arg("file");
  String path = "/encounters/" + filename;
  if (!SD.exists(path)) path = "/" + filename;

  if (!SD.exists(path)) {
    sendCorsHeaders();
    server.send(404, "text/plain", "Arquivo nao encontrado no SD");
    return;
  }

  File f = SD.open(path, FILE_READ);
  sendCorsHeaders();
  server.streamFile(f, "application/json");
  f.close();
}

void handleSaveEncounter() {
  if (!sdAvailable) {
    sendCorsHeaders();
    server.send(400, "text/plain", "Cartao SD nao disponivel");
    return;
  }
  if (!server.hasArg("file") || !server.hasArg("plain")) {
    sendCorsHeaders();
    server.send(400, "text/plain", "Dados faltando");
    return;
  }
  String filename = server.arg("file");
  if (!filename.endsWith(".json")) filename += ".json";
  String path = "/encounters/" + filename;

  if (SD.exists(path)) {
    SD.remove(path);
  }

  File f = SD.open(path, FILE_WRITE);
  if (!f) {
    sendCorsHeaders();
    server.send(500, "text/plain", "Falha ao gravar arquivo no SD");
    return;
  }
  f.print(server.arg("plain"));
  f.close();
  Serial.println("[SD] Encontro salvo com sucesso: " + path);
  sendCorsHeaders();
  server.send(200, "text/plain", "Encontro salvo com sucesso!");
}

void handleDeleteEncounter() {
  if (!sdAvailable || !server.hasArg("file")) {
    sendCorsHeaders();
    server.send(400, "text/plain", "Parametro invalido");
    return;
  }
  String filename = server.arg("file");
  String path = "/encounters/" + filename;
  if (!SD.exists(path)) path = "/" + filename;

  if (SD.exists(path)) {
    if (SD.remove(path)) {
      Serial.println("[SD] Encontro removido: " + path);
      sendCorsHeaders();
      server.send(200, "text/plain", "Encontro excluido com sucesso!");
      return;
    }
  }
  sendCorsHeaders();
  server.send(404, "text/plain", "Arquivo nao encontrado ou erro ao excluir");
}

// --- Leitura do Touch XPT2046 e Botão BOOT ---
void checkInput() {
  if (!inCombat || combatants.empty()) return;

  if (digitalRead(PIN_BTN_BOOT) == LOW) {
    if (millis() - lastActionTime > 400) {
      lastActionTime = millis();
      handleNext();
    }
    return;
  }

  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    if (p.z > 50 && p.x > 100 && p.y > 100) {
      if (millis() - lastActionTime > 400) {
        lastActionTime = millis();
        if (p.x < 2000) {
          handlePrev();
        } else {
          handleNext();
        }
      }
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);
  pinMode(PIN_BTN_BOOT, INPUT_PULLUP);

  tft.init();
  tft.setRotation(0);
  tft.fillScreen(TFT_BLACK);

  // Inicializa Touch via HSPI
  touchSpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSpi);
  ts.setRotation(0);

  // Inicializa Cartão MicroSD via VSPI
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (SD.begin(SD_CS, sdSPI, 20000000)) {
    sdAvailable = true;
    Serial.println("[SD] Cartao MicroSD detectado e pronto!");
    if (!SD.exists("/encounters")) {
      SD.mkdir("/encounters");
    }
    if (!SD.exists("/images")) {
      SD.mkdir("/images");
    }
  } else {
    sdAvailable = false;
    Serial.println("[SD] Cartao MicroSD nao detectado (operando normalmente via Flash).");
  }

  loadCombatantsFromNVS();

  prefs.begin("dnd_wifi", true);
  String currentSsid = prefs.getString("ssid", default_ssid);
  String currentPass = prefs.getString("pass", default_pass);
  prefs.end();

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("Conectando ao Wi-Fi...", tft.width() / 2, 70, 2);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  tft.drawCentreString(currentSsid.substring(0, 14), tft.width() / 2, 95, 2);

  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_STA);
  WiFi.begin(currentSsid.c_str(), currentPass.c_str());

  Serial.println("Tentando conectar em: " + currentSsid);

  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 24) {
    delay(500);
    Serial.print(".");
    tentativas++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    isApMode = false;
    Serial.println("\nWi-Fi Conectado!");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    isApMode = true;
    Serial.println("\nFalha ao conectar no roteador! Entrando em Modo AP...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP("DnD-Config", "d20nat20");
    Serial.print("AP IP: ");
    Serial.println(WiFi.softAPIP());
  }

  // Rotas HTTP
  server.on("/", handleRoot);
  server.on("/favicon.ico", handleFavicon);
  server.on("/add", handleAdd);
  server.on("/delete", handleDelete);
  server.on("/edit", handleEdit);
  server.on("/quick_init", handleQuickInit);
  server.on("/toggle_active", handleToggleActive);
  server.on("/timer", handleTimer);
  server.on("/start", handleStart);
  server.on("/next", handleNext);
  server.on("/prev", handlePrev);
  server.on("/end", handleEnd);
  server.on("/clear", handleClear);
  server.on("/status", handleStatus);
  server.on("/set_wifi", handleSetWifi);

  // Rotas do Cartão SD
  server.on("/list_encounters", handleListEncounters);
  server.on("/get_encounter", handleGetEncounter);
  server.on("/save_encounter", HTTP_POST, handleSaveEncounter);
  server.on("/delete_encounter", handleDeleteEncounter);
  server.on("/list_images", handleListImages);

  // Rota de Upload de Imagem
  server.on("/upload_image", HTTP_POST, []() {
    sendCorsHeaders();
    server.send(200, "text/plain", "Imagem enviada e salva no cartao SD com sucesso!");
  }, handleFileUpload);

  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("Servidor pronto!");

  updateDisplay();
}

void loop() {
  server.handleClient();
  checkInput();

  if (timerRunning) {
    if (millis() >= timerEndMillis) {
      timerRunning = false;
      timerExpired = true;
      timerExpiredMillis = millis();
      updateDisplay();
    } else if (millis() - lastTimerSecondUpdate >= 1000) {
      lastTimerSecondUpdate = millis();
      updateDisplay();
    }
  } else if (timerExpired) {
    if (millis() - timerExpiredMillis > 5000) {
      timerExpired = false;
      updateDisplay();
    }
  }
}