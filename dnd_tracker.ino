#include <WiFi.h>
#include <WebServer.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include <vector>
#include <algorithm>

// --- Credenciais Padrão de Wi-Fi (usadas caso não haja nada salvo na Flash) ---
const char* default_ssid = "Pedro";
const char* default_pass = "internetico";

// --- Configuração dos Pinos da Placa CYD ---
#define XPT2046_IRQ  36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK  25
#define XPT2046_CS   33
#define TFT_BL       21
#define PIN_BTN_BOOT 0

// Instâncias
TFT_eSPI tft = TFT_eSPI();
SPIClass touchSpi = SPIClass(HSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
WebServer server(80);
Preferences prefs;

struct Combatant {
  String name;
  String icon;
  int initiative;
};

std::vector<Combatant> combatants;
bool inCombat = false;
int currentIndex = 0;
int currentRound = 1;
unsigned long lastActionTime = 0;
bool isApMode = false;

// --- Persistência de Dados (NVS / Flash) ---
void saveCombatantsToNVS() {
  prefs.begin("dnd_data", false);
  String data = "";
  for (size_t i = 0; i < combatants.size(); i++) {
    data += combatants[i].name + "|" + combatants[i].icon + "|" + String(combatants[i].initiative) + "\n";
  }
  prefs.putString("list", data);
  prefs.end();
  Serial.println("[NVS] Combatentes salvos na memoria Flash.");
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
    if (p1 != -1 && p2 != -1) {
      Combatant c;
      c.name = line.substring(0, p1);
      c.icon = line.substring(p1 + 1, p2);
      c.initiative = line.substring(p2 + 1).toInt();
      combatants.push_back(c);
    }
  }
  Serial.println("[NVS] Combatentes carregados da memoria Flash: " + String(combatants.size()));
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

// --- Desenhos dos Ícones de Classe/Monstro no Display ---
void drawClassIcon(int cx, int cy, String icon) {
  tft.drawRoundRect(cx - 22, cy - 22, 44, 44, 6, TFT_WHITE);

  if (icon == "boss") {
    tft.fillRect(cx - 14, cy + 4, 28, 6, TFT_GOLD);
    tft.fillTriangle(cx - 14, cy + 4, cx - 14, cy - 8, cx - 4, cy + 4, TFT_GOLD);
    tft.fillTriangle(cx - 6, cy + 4, cx, cy - 12, cx + 6, cy + 4, TFT_GOLD);
    tft.fillTriangle(cx + 4, cy + 4, cx + 14, cy - 8, cx + 14, cy + 4, TFT_GOLD);
    tft.fillCircle(cx, cy - 12, 2, TFT_RED);
  } else if (icon == "monster") {
    tft.fillCircle(cx, cy - 2, 13, TFT_MAGENTA);
    tft.fillRect(cx - 8, cy + 4, 16, 7, TFT_MAGENTA);
    tft.fillCircle(cx - 5, cy - 3, 3, TFT_BLACK);
    tft.fillCircle(cx + 5, cy - 3, 3, TFT_BLACK);
    tft.fillRect(cx - 5, cy + 8, 10, 2, TFT_WHITE);
  } else if (icon == "warrior") {
    tft.drawLine(cx - 12, cy - 12, cx + 12, cy + 12, TFT_ORANGE);
    tft.drawLine(cx + 12, cy - 12, cx - 12, cy + 12, TFT_ORANGE);
    tft.fillCircle(cx, cy, 4, TFT_WHITE);
  } else if (icon == "barbarian") {
    tft.fillRect(cx - 2, cy - 14, 4, 28, 0x9A60);
    tft.fillTriangle(cx - 2, cy - 10, cx - 14, cy - 14, cx - 14, cy - 2, TFT_RED);
    tft.fillTriangle(cx + 2, cy - 10, cx + 14, cy - 14, cx + 14, cy - 2, TFT_RED);
  } else if (icon == "paladin") {
    tft.fillTriangle(cx - 12, cy - 10, cx + 12, cy - 10, cx, cy + 14, TFT_GOLD);
    tft.fillRect(cx - 2, cy - 8, 4, 16, TFT_WHITE);
    tft.fillRect(cx - 6, cy - 4, 12, 4, TFT_WHITE);
  } else if (icon == "cleric") {
    tft.fillRect(cx - 3, cy - 14, 6, 28, TFT_GOLD);
    tft.fillRect(cx - 11, cy - 6, 22, 6, TFT_GOLD);
  } else if (icon == "druid") {
    tft.fillCircle(cx, cy - 2, 10, TFT_GREEN);
    tft.fillRect(cx - 2, cy + 6, 4, 8, 0x9A60);
    tft.drawLine(cx, cy - 8, cx, cy + 6, TFT_DARKGREEN);
  } else if (icon == "wizard") {
    tft.fillRect(cx - 12, cy - 10, 24, 20, TFT_CYAN);
    tft.drawLine(cx - 8, cy - 5, cx + 8, cy - 5, TFT_BLACK);
    tft.drawLine(cx - 8, cy, cx + 8, cy, TFT_BLACK);
    tft.drawLine(cx - 8, cy + 5, cx + 4, cy + 5, TFT_BLACK);
  } else if (icon == "sorcerer") {
    tft.fillCircle(cx, cy, 12, TFT_PURPLE);
    tft.drawCircle(cx, cy, 15, TFT_MAGENTA);
    tft.fillCircle(cx - 3, cy - 3, 3, TFT_WHITE);
  } else if (icon == "warlock") {
    tft.fillCircle(cx, cy, 12, TFT_NAVY);
    tft.fillCircle(cx, cy, 6, TFT_VIOLET);
    tft.fillCircle(cx, cy, 2, TFT_BLACK);
  } else if (icon == "rogue") {
    tft.fillTriangle(cx, cy - 14, cx - 4, cy + 4, cx + 4, cy + 4, TFT_SILVER);
    tft.fillRect(cx - 8, cy + 4, 16, 3, TFT_GOLD);
    tft.fillRect(cx - 2, cy + 7, 4, 7, TFT_RED);
  } else if (icon == "ranger") {
    tft.drawCircle(cx - 4, cy, 12, TFT_DARKGREEN);
    tft.drawLine(cx - 4, cy - 12, cx - 4, cy + 12, TFT_WHITE);
    tft.drawLine(cx - 10, cy, cx + 12, cy, TFT_YELLOW);
  } else if (icon == "monk") {
    tft.fillCircle(cx, cy - 2, 9, TFT_ORANGE);
    tft.fillRect(cx - 10, cy + 7, 20, 5, TFT_BLACK);
    tft.fillRect(cx - 2, cy + 12, 4, 6, TFT_BLACK);
  } else if (icon == "bard") {
    tft.fillCircle(cx - 4, cy + 6, 5, TFT_PINK);
    tft.fillRect(cx, cy - 10, 3, 16, TFT_PINK);
    tft.fillRect(cx - 10, cy - 10, 8, 4, TFT_PINK);
  } else if (icon == "artificer") {
    tft.fillCircle(cx, cy, 11, TFT_SILVER);
    tft.fillCircle(cx, cy, 4, TFT_BLACK);
    tft.fillRect(cx - 2, cy - 14, 4, 28, TFT_SILVER);
    tft.fillRect(cx - 14, cy - 2, 28, 4, TFT_SILVER);
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

  tft.fillRect(0, 0, w, 24, TFT_NAVY);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawString("ROD. " + String(currentRound), 6, 4, 2);
  tft.drawRightString(String(currentIndex + 1) + "/" + String(combatants.size()), w - 6, 4, 2);

  int iconY = 56;
  drawClassIcon(w / 2, iconY, current.icon);

  tft.setTextColor(TFT_SILVER, TFT_BLACK);
  tft.drawCentreString("[" + getClassName(current.icon) + "]", w / 2, iconY + 28, 2);

  uint16_t nameColor = (current.icon == "boss" ? TFT_GOLD : (current.icon == "monster" ? TFT_RED : TFT_GREEN));
  tft.setTextColor(nameColor, TFT_BLACK);
  int nameFont = (w < 160) ? 2 : 4;
  tft.drawCentreString(current.name.substring(0, 10), w / 2, iconY + 50, nameFont);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawCentreString("Iniciativa: " + String(current.initiative), w / 2, iconY + 80, 2);

  tft.drawLine(10, iconY + 104, w - 10, iconY + 104, TFT_DARKGREY);
  int nextIndex = (currentIndex + 1) % combatants.size();
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawCentreString("Prox: " + combatants[nextIndex].name + " (" + String(combatants[nextIndex].initiative) + ")", w / 2, iconY + 112, 2);

  int btnH = 36;
  int btnY = h - btnH - 6;

  tft.fillRoundRect(4, btnY, (w / 2) - 6, btnH, 5, 0x4208);
  tft.setTextColor(TFT_WHITE, 0x4208);
  tft.drawCentreString("<< VOLTAR", (w / 4) + 1, btnY + 10, 2);

  tft.fillRoundRect((w / 2) + 2, btnY, (w / 2) - 6, btnH, 5, TFT_NAVY);
  tft.setTextColor(TFT_YELLOW, TFT_NAVY);
  tft.drawCentreString("AVANCAR >>", (w * 3 / 4) - 1, btnY + 10, 2);
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
    .btn-sec { background: #37474f; color: #eee; font-size: 0.95rem; margin: 6px 0; display: block; text-align: center; border-radius: 6px; text-decoration: none; padding: 12px; box-sizing: border-box; font-weight: bold; cursor: pointer; }
    .combatant-item { display: flex; justify-content: space-between; align-items: center; padding: 8px 10px; border-bottom: 1px solid #2d2d2d; }
    .item-actions button { width: auto; padding: 4px 8px; margin: 0 2px; font-size: 0.85rem; background: #333; color: #fff; border: 1px solid #555; }
    .status-badge { font-weight: bold; color: #d4af37; text-align: center; font-size: 1.1rem; margin-bottom: 10px; }
    .collapsible { background: #292929; color: #aaa; padding: 8px; border-radius: 5px; cursor: pointer; text-align: center; font-size: 0.9rem; margin-top: 10px; border: 1px dashed #444; }
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

  <!-- ADICIONAR COMBATENTE -->
  <div class="card">
    <h3 style="margin: 0 0 8px 0;">Adicionar Combatente</h3>
    <input type="text" id="name" placeholder="Nome do Personagem ou Monstro">
    <select id="icon">
      <option value="artificer">🛠️ Artífice</option>
      <option value="barbarian">🪓 Bárbaro</option>
      <option value="bard">🪕 Bardo</option>
      <option value="warlock">👁️‍🗨️ Bruxo</option>
      <option value="cleric">✝️ Clérigo</option>
      <option value="druid">🌿 Druida</option>
      <option value="sorcerer">🔮 Feiticeiro</option>
      <option value="warrior" selected>⚔️ Guerreiro</option>
      <option value="rogue">🗡️ Ladino</option>
      <option value="wizard">📜 Mago</option>
      <option value="monk">🥋 Monge</option>
      <option value="paladin">🛡️ Paladino</option>
      <option value="ranger">🏹 Ranger (Patrulheiro)</option>
      <option value="monster">👾 Monstro</option>
      <option value="boss">👑 Boss</option>
    </select>
    <input type="number" id="init" placeholder="Iniciativa (ex: 18)">
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
    
    <!-- Input com Label nativo compatível com Android -->
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

    function loadStatus() {
      fetch('/status')
        .then(r => r.json())
        .then(data => {
          currentList = data.list;
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

          let listHtml = '';
          data.list.forEach((c, idx) => {
            let isCurrent = (data.inCombat && idx === data.currentIndex) ? 'style="color:#00e676; font-weight:bold; background:#223322;"' : '';
            listHtml += `<div class="combatant-item" ${isCurrent}>
              <span>${labels[c.icon] || '👤'} ${c.name} (${c.initiative})</span>
              <div class="item-actions">
                <button onclick="editItem(${idx}, '${encodeURIComponent(c.name)}', ${c.initiative}, '${c.icon}')">✏️</button>
                <button onclick="deleteItem(${idx})">❌</button>
              </div>
            </div>`;
          });
          document.getElementById('list').innerHTML = listHtml || '<p style="color:#666">Nenhum combatente cadastrado.</p>';
        })
        .catch(e => console.log('Aguardando servidor...'));
    }

    function action(endpoint) {
      fetch(endpoint).then(() => loadStatus());
    }

    function addCombatant() {
      let name = document.getElementById('name').value.trim();
      let icon = document.getElementById('icon').value;
      let init = document.getElementById('init').value;
      if (!name || !init) return alert('Preencha o nome e a iniciativa!');

      fetch(`/add?name=${encodeURIComponent(name)}&icon=${icon}&init=${init}`)
        .then(() => {
          document.getElementById('name').value = '';
          document.getElementById('init').value = '';
          loadStatus();
        });
    }

    function deleteItem(idx) {
      if (confirm('Remover este combatente?')) {
        fetch('/delete?idx=' + idx).then(() => loadStatus());
      }
    }

    function editItem(idx, encodedName, currentInit, currentIcon) {
      let currentName = decodeURIComponent(encodedName);
      let name = prompt("Nome:", currentName);
      if (name === null) return;
      let init = prompt("Iniciativa:", currentInit);
      if (init === null) return;
      fetch(`/edit?idx=${idx}&name=${encodeURIComponent(name)}&icon=${currentIcon}&init=${init}`).then(() => loadStatus());
    }

    // Exportar JSON (Baixar)
    function exportJson() {
      let dataStr = "data:text/json;charset=utf-8," + encodeURIComponent(JSON.stringify(currentList, null, 2));
      let a = document.createElement('a');
      a.setAttribute("href", dataStr);
      a.setAttribute("download", "dnd_iniciativa.json");
      a.click();
    }

    // Processamento Seguro e Sequencial do JSON
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
          await fetch(`/add?name=${name}&icon=${icon}&init=${init}`);
        }
        loadStatus();
        alert('Personagens importados com sucesso!');
      } catch (err) {
        alert('Erro ao enviar para o ESP32: ' + err.message);
      }
    }

    // Importar via Arquivo (compatível com Android)
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

    // Importar colando o texto (alternativa à prova de falhas no celular)
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

    setInterval(loadStatus, 3000);
    loadStatus();
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
      saveCombatantsToNVS();
      updateDisplay();
    }
  }
  sendCorsHeaders();
  server.send(200, "text/plain", "OK");
}

void handleStart() {
  if (combatants.empty()) {
    sendCorsHeaders();
    server.send(400, "text/plain", "Lista vazia");
    return;
  }
  std::sort(combatants.begin(), combatants.end(), [](const Combatant &a, const Combatant &b) {
    return a.initiative > b.initiative;
  });

  currentIndex = 0;
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
  currentIndex++;
  if (currentIndex >= (int)combatants.size()) {
    currentIndex = 0;
    currentRound++;
  }
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
  if (currentIndex > 0) {
    currentIndex--;
  } else {
    if (currentRound > 1) {
      currentRound--;
      currentIndex = combatants.size() - 1;
    } else {
      currentIndex = 0;
    }
  }
  updateDisplay();
  sendCorsHeaders();
  server.send(200, "text/plain", "Anterior");
}

void handleEnd() {
  inCombat = false;
  currentIndex = 0;
  currentRound = 1;
  updateDisplay();
  sendCorsHeaders();
  server.send(200, "text/plain", "Finalizado");
}

void handleClear() {
  combatants.clear();
  inCombat = false;
  currentIndex = 0;
  currentRound = 1;
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
  json += "\"list\":[";
  for (size_t i = 0; i < combatants.size(); i++) {
    json += "{\"name\":\"" + combatants[i].name + "\",";
    json += "\"icon\":\"" + combatants[i].icon + "\",";
    json += "\"initiative\":" + String(combatants[i].initiative) + "}";
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

  touchSpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(touchSpi);
  ts.setRotation(0);

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
  server.on("/start", handleStart);
  server.on("/next", handleNext);
  server.on("/prev", handlePrev);
  server.on("/end", handleEnd);
  server.on("/clear", handleClear);
  server.on("/status", handleStatus);
  server.on("/set_wifi", handleSetWifi);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("Servidor pronto!");

  updateDisplay();
}

void loop() {
  server.handleClient();
  checkInput();
}