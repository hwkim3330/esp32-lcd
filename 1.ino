// ===== deps =====
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_sntp.h>

#include <LovyanGFX.hpp>
#define LGFX_AUTODETECT
#include <LGFX_AUTODETECT.hpp>

#include <U8g2lib.h>                  // 저용량 한글 표시용
static lgfx::U8g2font u8g2_kor(u8g2_font_unifont_t_korean2);

// ===== user config =====
static const char* WIFI_SSID = "iptime";
static const char* WIFI_PASS = "";                 // open이면 빈 문자열

static const char* DOORAY_TOKEN = "YOUR_TOKEN_HERE";  // ★ 개인 토큰 입력
static const char* DOORAY_BASE  = "https://api.dooray.com";

// 방 목록 (ID 고정, 라벨 한글)
struct Room { const char* id; const char* label; };
Room ROOMS[] = {
  {"3868297860122914233", "A동-6층 소회의실3"},
  {"3868312518617103681", "B동-6층 소회의실1"},
  {"3868312634809468080", "B동-6층 소회의실2"},
  {"3868312748489680534", "B동-6층 중회의실1"}
};
constexpr int ROOM_COUNT = sizeof(ROOMS) / sizeof(ROOMS[0]);
int curRoom = 0;

// ===== gfx =====
static LGFX lcd;

// 레이아웃 상수
int TOP_H = 28;             // 상단 bar(시계/날짜)
int NAV_H = 40;             // 방 라벨/화살표 영역
int LIST_Y_POS;             // 목록 시작 y
int LIST_HEIGHT;            // 목록 높이

// 버튼들
struct Btn { int x, y, w, h; const char* text; uint16_t col; };
Btn btnPrev, btnNext, btnRefresh;

static inline bool hit(const Btn& b, int32_t x, int32_t y) {
  return (x >= b.x && x <= b.x + b.w && y >= b.y && y <= b.y + b.h);
}

void drawBtn(const Btn& b) {
  lcd.fillRoundRect(b.x, b.y, b.w, b.h, 10, b.col);
  lcd.setFont(&u8g2_kor);
  lcd.setTextColor(TFT_BLACK);
  lcd.setTextDatum(MC_DATUM);
  if (b.text && *b.text) lcd.drawString(b.text, b.x + b.w/2, b.y + b.h/2);
}

// ===== time helpers =====
bool timeReady(){
  return time(nullptr) > 1700000000; // ~2023-11 이후면 OK
}

static bool sntpStarted = false;
static void startSNTPOnce() {
  if (sntpStarted) return;
  setenv("TZ", "KST-9", 1); tzset();
  configTzTime("KST-9",
               "kr.pool.ntp.org",
               "time.google.com",
               "time.cloudflare.com");
  sntpStarted = true;
}

// UTC용 mktime 대체 (timegm() 없는 보드 호환)
time_t mktime_utc(struct tm* tmv) {
  char* oldtz = getenv("TZ");
  setenv("TZ", "UTC0", 1);
  tzset();
  time_t t = mktime(tmv);
  if (oldtz) setenv("TZ", oldtz, 1);
  else unsetenv("TZ");
  tzset();
  return t;
}

// HTTP Date 폴백: Dooray 서버의 Date 헤더로 시간 세팅
bool httpDateFallback() {
  WiFiClientSecure cli; cli.setInsecure();
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(cli, String(DOORAY_BASE) + "/");
  const char* keys[] = {"Date"};
  http.collectHeaders(keys, 1);
  int code = http.GET();
  if (code <= 0) { http.end(); return false; }
  String date = http.header("Date"); // "Tue, 29 Jul 2025 04:37:12 GMT"
  http.end();
  if (date.length() < 10) return false;

  int d, Y, h, m, s; char Mon[4];
  if (sscanf(date.c_str(), "%*3s, %d %3s %d %d:%d:%d GMT", &d, Mon, &Y, &h, &m, &s) != 6) return false;

  const char* tbl = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* p = strstr(tbl, Mon); if (!p) return false;
  int mon = (p - tbl) / 3 + 1;

  struct tm tmv = {};
  tmv.tm_year = Y - 1900; tmv.tm_mon = mon - 1; tmv.tm_mday = d;
  tmv.tm_hour = h; tmv.tm_min = m; tmv.tm_sec = s;
  time_t gmt = mktime_utc(&tmv);
  if (gmt <= 0) return false;

  struct timeval tv = { gmt, 0 };
  settimeofday(&tv, nullptr);
  setenv("TZ", "KST-9", 1); tzset();
  return true;
}

void ensureTime(){
  if (timeReady()) return;
  startSNTPOnce();

  struct tm ti;
  for (int i=0;i<40;i++){ if (getLocalTime(&ti, 500)) return; }
  if (httpDateFallback()) return;
  for (int i=0;i<20;i++){ if (getLocalTime(&ti, 500)) return; }
}

// ===== Dooray API =====
String authHeader(){ return String("dooray-api ") + DOORAY_TOKEN; }

void httpSetup(HTTPClient& http, WiFiClientSecure& cli, const String& url){
  cli.setInsecure(); // 데모: 운영은 CA 고정 권장
  http.setConnectTimeout(15000);
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.useHTTP10(true);
  http.addHeader("Authorization", authHeader());
  http.addHeader("Accept", "application/json");
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "ESP32-DOORAY/1.0");
  http.begin(cli, url);
}

struct Resv { String subject, start, end, who; };
Resv resv[16]; int resvCount = 0;

// KST 기준 오늘 00:00~24:00
void kstDayRangeISO(String& timeMin, String& timeMax) {
  time_t now = time(nullptr);
  struct tm lt; localtime_r(&now, &lt);
  lt.tm_hour = 0; lt.tm_min=0; lt.tm_sec=0;
  time_t start = mktime(&lt);
  time_t end   = start + 24*3600;

  char buf1[40], buf2[40];
  strftime(buf1,sizeof(buf1),"%Y-%m-%dT%H:%M:%S+09:00",&lt);
  struct tm lt2; localtime_r(&end, &lt2);
  strftime(buf2,sizeof(buf2),"%Y-%m-%dT%H:%M:%S+09:00",&lt2);

  timeMin = buf1; timeMax = buf2;
}

String hhmm(const String& iso){
  int tpos = iso.indexOf('T'); if (tpos < 0) return "--:--";
  String h = iso.substring(tpos+1, tpos+3);
  String m = iso.substring(tpos+4, tpos+6);
  return h + ":" + m;
}

// Dooray: GET /reservation/v1/resource-reservations?resourceIds=...&timeMin=...&timeMax=...
bool fetchResvToday(const char* roomId, String& errOut){
  resvCount = 0;
  ensureTime();
  if (!timeReady()){ errOut = "시간 미동기화"; return false; }

  String tMin, tMax; kstDayRangeISO(tMin, tMax);

  String url = String(DOORAY_BASE) + "/reservation/v1/resource-reservations";
  url += "?resourceIds=" + String(roomId);
  url += "&timeMin=" + tMin;
  url += "&timeMax=" + tMax;

  WiFiClientSecure cli; HTTPClient http;
  httpSetup(http, cli, url);
  int code = http.GET();
  String body = (code>0) ? http.getString() : "";
  http.end();

  Serial.printf("[GET resv] code=%d len=%d\n", code, body.length());
  if (code!=200 || body.length()==0){ errOut = "HTTP "+String(code); return false; }

  // 필요한 필드만 파싱 + 예약자명 포함
  StaticJsonDocument<768> filter;
  JsonObject item = filter["result"].createNestedObject();
  item["subject"] = true;
  item["startedAt"] = true;
  item["endedAt"] = true;
  item["users"]["from"]["type"] = true;
  item["users"]["from"]["member"]["name"] = true;
  item["users"]["from"]["emailUser"]["name"] = true;
  item["users"]["from"]["emailUser"]["emailAddress"] = true;

  DynamicJsonDocument doc(16384);
  DeserializationError e = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (e){ errOut = String("JSON ")+e.c_str(); return false; }

  JsonArray arr = doc["result"].as<JsonArray>();
  if (arr.isNull()){ errOut = "no result"; return false; }

  for (JsonVariant v : arr){
    if (resvCount >= (int)(sizeof(resv)/sizeof(resv[0]))) break;
    Resv r;
    r.subject = (const char*) (v["subject"] | "");
    if (r.subject.length()==0) r.subject = "(제목 없음)";
    r.start   = (const char*) (v["startedAt"] | "");
    r.end     = (const char*) (v["endedAt"]   | "");

    const char* ftype = v["users"]["from"]["type"] | "";
    String who;
    if (strcmp(ftype,"member")==0){
      who = (const char*) (v["users"]["from"]["member"]["name"] | "");
    } else if (strcmp(ftype,"emailUser")==0){
      who = (const char*) (v["users"]["from"]["emailUser"]["name"] | "");
      if (who.length()==0) who = (const char*) (v["users"]["from"]["emailUser"]["emailAddress"] | "");
    }
    if (who.length()==0) who = "예약자";

    r.who = who;
    resv[resvCount++] = r;
  }
  return true;
}

// ===== UI =====
String lastClock;
const char* KWD[7] = {"일","월","화","수","목","금","토"};

void drawTopBar(bool forceAll=false){
  lcd.fillRect(0, 0, lcd.width(), TOP_H, TFT_WHITE);
  lcd.drawFastHLine(0, TOP_H-1, lcd.width(), TFT_LIGHTGREY);

  ensureTime();
  time_t now = time(nullptr);
  if (now < 1700000000) return;
  struct tm lt; localtime_r(&now, &lt);

  char clockBuf[16]; strftime(clockBuf,sizeof(clockBuf),"%H:%M:%S",&lt);
  lcd.setFont(&u8g2_kor);
  lcd.setTextColor(TFT_BLACK, TFT_WHITE);
  lcd.setTextDatum(TL_DATUM);
  lcd.drawString(clockBuf, 6, 6);

  char dateBuf[24]; strftime(dateBuf,sizeof(dateBuf),"%Y-%m-%d",&lt);
  String dateK = String(dateBuf) + "(" + KWD[lt.tm_wday] + ")";
  lcd.setTextDatum(TR_DATUM);
  lcd.drawString(dateK, lcd.width()-6, 6);
}

static void drawArrowInBtn(const Btn& b, bool left){
  int cx = b.x + b.w/2;
  int cy = b.y + b.h/2;
  int hw = b.w/3;      // 가로 삼각형 너비/2
  int hh = b.h/3;      // 세로 높이/2
  if (left){
    lcd.fillTriangle(cx + hw/2, cy - hh,  cx + hw/2, cy + hh,  cx - hw, cy, TFT_BLACK);
  } else {
    lcd.fillTriangle(cx - hw/2, cy - hh,  cx - hw/2, cy + hh,  cx + hw, cy, TFT_BLACK);
  }
}

void drawNavBar(){
  lcd.fillRect(0, TOP_H, lcd.width(), NAV_H, TFT_WHITE);
  lcd.drawFastHLine(0, TOP_H+NAV_H-1, lcd.width(), TFT_LIGHTGREY);

  // 버튼 위치
  btnPrev    = { 8,  TOP_H + 6, 40, NAV_H-12, "", (uint16_t)0xD6BA };         // 글자대신 도형
  btnNext    = { lcd.width()-48, TOP_H + 6, 40, NAV_H-12, "", (uint16_t)0xD6BA };
  btnRefresh = { lcd.width()/2 - 38, lcd.height()-34, 76, 26, "새로고침", (uint16_t)0xE6FF };

  drawBtn(btnPrev);   drawArrowInBtn(btnPrev, true);
  drawBtn(btnNext);   drawArrowInBtn(btnNext, false);

  // 방 라벨 칩
  int chipW = lcd.width() - (btnPrev.w + btnNext.w + 32);
  int chipX = btnPrev.x + btnPrev.w + 8;
  int chipY = TOP_H + 6;
  lcd.fillRoundRect(chipX, chipY, chipW, NAV_H-12, 12, TFT_DARKGREEN);
  lcd.setTextColor(TFT_WHITE);
  lcd.setFont(&u8g2_kor);
  lcd.setTextDatum(MC_DATUM);
  lcd.drawString(ROOMS[curRoom].label, chipX + chipW/2, chipY + (NAV_H-12)/2);

  // 하단 새로고침 버튼
  drawBtn(btnRefresh);
}

void drawListHeader(){
  lcd.setFont(&u8g2_kor);
  lcd.setTextColor(TFT_DARKGREY, TFT_WHITE);
  lcd.setTextDatum(TL_DATUM);
  lcd.drawString("오늘 예약", 8, LIST_Y_POS - 18);
}

void drawList(){
  lcd.fillRect(0, LIST_Y_POS-2, lcd.width(), LIST_HEIGHT+4, TFT_WHITE);
  drawListHeader();

  lcd.setFont(&u8g2_kor);
  lcd.setTextDatum(TL_DATUM);
  lcd.setTextColor(TFT_BLACK, TFT_WHITE);

  int y = LIST_Y_POS;
  const int lineH = 20;
  if (resvCount == 0){
    lcd.drawString("없음", 10, y);
    return;
  }
  for (int i=0;i<resvCount && y <= LIST_Y_POS + LIST_HEIGHT - lineH; ++i){
    String who = resv[i].who.length() ? (" / " + resv[i].who) : "";
    String line = hhmm(resv[i].start) + "-" + hhmm(resv[i].end) + "  " + resv[i].subject + who;
    if (line.length() > 48) line = line.substring(0, 48);
    lcd.drawString(line, 10, y);
    y += lineH;
  }
}

// 상단 시계만 부드럽게 갱신
void updateClockIfChanged(){
  if (!timeReady()) return;
  time_t now = time(nullptr);
  struct tm lt; localtime_r(&now, &lt);
  char buf[16]; strftime(buf,sizeof(buf),"%H:%M:%S",&lt);
  String cur(buf);
  if (cur == lastClock) return;
  lastClock = cur;
  lcd.fillRect(0, 0, 120, TOP_H, TFT_WHITE);
  lcd.setFont(&u8g2_kor);
  lcd.setTextColor(TFT_BLACK, TFT_WHITE);
  lcd.setTextDatum(TL_DATUM);
  lcd.drawString(cur, 6, 6);
}

// 데이터 + 화면 전체 갱신
void refetchAndRedraw(){
  drawTopBar(true);
  drawNavBar();

  // 상태 표시
  lcd.fillRect(0, LIST_Y_POS-2, lcd.width(), 20, TFT_WHITE);
  lcd.setFont(&u8g2_kor);
  lcd.setTextColor(TFT_DARKGREY, TFT_WHITE);
  lcd.setTextDatum(TL_DATUM);
  lcd.drawString("불러오는 중...", 10, LIST_Y_POS-2);

  String err;
  if (!fetchResvToday(ROOMS[curRoom].id, err)){
    lcd.fillRect(0, LIST_Y_POS-2, lcd.width(), 20, TFT_WHITE);
    lcd.setTextColor(TFT_RED, TFT_WHITE);
    lcd.drawString(("불러오기 실패: " + err).substring(0, 42), 10, LIST_Y_POS-2);
  } else {
    lcd.fillRect(0, LIST_Y_POS-2, lcd.width(), 20, TFT_WHITE);
  }
  drawList();
}

// ===== setup/loop =====
void setup(){
  Serial.begin(115200);

  lcd.init();
  lcd.setRotation(1);          // 필요시 0/1/2/3 조정
  lcd.setBrightness(200);
  lcd.fillScreen(TFT_WHITE);

  // 레이아웃 계산
  LIST_Y_POS   = TOP_H + NAV_H + 14;
  LIST_HEIGHT  = lcd.height() - LIST_Y_POS - 40;

  // Wi-Fi
  lcd.setFont(&u8g2_kor);
  lcd.setTextColor(TFT_BLACK, TFT_WHITE);
  lcd.setTextDatum(TL_DATUM);
  lcd.drawString("Wi-Fi 연결 중...", 10, 10);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis()-t0 < 20000){
    delay(200);
  }
  lcd.fillRect(0, 10, lcd.width(), 16, TFT_WHITE);
  if (WiFi.status() != WL_CONNECTED){
    lcd.setTextColor(TFT_RED, TFT_WHITE);
    lcd.drawString("Wi-Fi 실패", 10, 10);
  } else {
    lcd.setTextColor(TFT_BLACK, TFT_WHITE);
    lcd.drawString("IP: " + WiFi.localIP().toString(), 10, 10);
  }

  ensureTime();
  drawTopBar(true);
  drawNavBar();

  // 첫 로드
  refetchAndRedraw();
}

void loop(){
  updateClockIfChanged();

  int32_t x=-1, y=-1;
  if (lcd.getTouch(&x, &y)){
    if (hit(btnPrev, x, y)){
      curRoom = (curRoom + ROOM_COUNT - 1) % ROOM_COUNT;  // 순환
      refetchAndRedraw();
      delay(200);
    } else if (hit(btnNext, x, y)){
      curRoom = (curRoom + 1) % ROOM_COUNT;               // 순환
      refetchAndRedraw();
      delay(200);
    } else if (hit(btnRefresh, x, y)){
      refetchAndRedraw();
      delay(150);
    }
  }
}
