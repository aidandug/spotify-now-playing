#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <FS.h>
#include <SPIFFS.h>
#include <JPEGDecoder.h>

#include "secrets.h"

#define TFT_CS    13
#define TFT_DC    12
#define TFT_RST   14

SPIClass spi = SPIClass(VSPI);
Adafruit_ST7735 tft = Adafruit_ST7735(&spi, TFT_CS, TFT_DC, TFT_RST);

String accessToken = "";
unsigned long lastTokenRefreshTime = 0;

String lastSong = "";
String currentSong = "";
String currentArtist = "";
int currentProgress = 0;
int totalDuration = 0;
unsigned long lastProgressUpdate = 0;
unsigned long lastFetchTime = 0;

int scrollSongX = 0;
int scrollArtistX = 0;
int songPixelWidth = 0;

unsigned long lastScrollTime = 0;
unsigned long scrollPauseStart = 0;
bool pauseScrolling = false;

void connectWiFi() {
  Serial.print("connecting to wifi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000) {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nwifi connected");
  } else {
    Serial.println("\nwifi failed, restarting...");
    delay(3000);
    ESP.restart();
  }
}

void setupSPIFFS() {
  if (!SPIFFS.begin(true)) {
    Serial.println("spiffs mount failed");
    return;
  }
  Serial.println("spiffs mounted successfully");
}

bool refreshAccessToken() {
  HTTPClient http;
  http.begin("https://accounts.spotify.com/api/token");
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  String body = "grant_type=refresh_token";
  body += "&refresh_token=" + String(REFRESH_TOKEN);
  body += "&client_id=" + String(CLIENT_ID);
  body += "&client_secret=" + String(CLIENT_SECRET);

  int httpResponseCode = http.POST(body);
  if (httpResponseCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, payload)) return false;
    accessToken = doc["access_token"].as<String>();
    lastTokenRefreshTime = millis();
    return true;
  }
  return false;
}

void fetchSpotifyData() {
  if (accessToken == "" || (millis() - lastTokenRefreshTime) > 3600000) {
    if (!refreshAccessToken()) return;
  }

  HTTPClient http;
  http.begin("https://api.spotify.com/v1/me/player/currently-playing");
  http.addHeader("Authorization", "Bearer " + accessToken);
  int httpResponseCode = http.GET();

  if (httpResponseCode == 200) {
    String payload = http.getString();
    StaticJsonDocument<1024> doc;
    if (deserializeJson(doc, payload)) return;

    currentSong = doc["item"]["name"].as<String>();
    currentArtist = doc["item"]["artists"][0]["name"].as<String>();
    String albumImageURL = doc["item"]["album"]["images"][2]["url"].as<String>();
    currentProgress = doc["progress_ms"];
    totalDuration = doc["item"]["duration_ms"];
    lastProgressUpdate = millis();

    if (currentSong != lastSong) {
      lastSong = currentSong;
      scrollSongX = tft.width();
      scrollArtistX = tft.width();
      animateScreenTransition();
      tft.fillScreen(ST77XX_BLACK);
      downloadAndDisplayAlbumArt(albumImageURL);

      int16_t x1, y1;
      uint16_t w, h;
      tft.setTextSize(2);
      tft.getTextBounds(currentSong, 0, 0, &x1, &y1, &w, &h);
      songPixelWidth = w;

      displayOnTFT();
    }
  } else if (httpResponseCode == 204) {
    displayNoContent();
  } else if (httpResponseCode == 401) {
    if (refreshAccessToken()) fetchSpotifyData();
  }
  http.end();
}

void drawScrollingText(const String& text, uint16_t color, int scrollX, int y, int textWidth) {
  tft.setTextSize(2);
  tft.setTextWrap(false);
  tft.setTextColor(color);

  tft.setCursor(scrollX, y);
  tft.print(text);
}

void displayOnTFT() {
  int songY = 110;
  int artistY = 125;
  int progressY = 145;

  tft.fillRect(0, songY, tft.width(), 16, ST77XX_BLACK);
  if (songPixelWidth > tft.width()) {
    drawScrollingText(currentSong, ST77XX_BLUE, scrollSongX, songY, songPixelWidth);
  } else {
    tft.setTextSize(2);
    tft.setTextWrap(false);
    tft.setTextColor(ST77XX_BLUE);
    int16_t sx, sy;
    uint16_t sw, sh;
    tft.getTextBounds(currentSong, 0, 0, &sx, &sy, &sw, &sh);
    tft.setCursor((tft.width() - sw) / 2, songY);
    tft.print(currentSong);
  }

  tft.fillRect(0, artistY, tft.width(), 16, ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextWrap(false);
  tft.setTextColor(ST77XX_WHITE);
  int16_t ax, ay;
  uint16_t aw, ah;
  tft.getTextBounds(currentArtist, 0, 0, &ax, &ay, &aw, &ah);
  tft.setCursor((tft.width() - aw) / 2, artistY);
  tft.print(currentArtist);

  updateProgressBar(currentProgress, totalDuration, progressY);
}

void updateProgressBar(int progress, int duration, int barY) {
  int barHeight = 5;
  int barX = 5;
  int barMaxWidth = tft.width() - 10;
  int barWidth = map(progress, 0, duration, 0, barMaxWidth);

  tft.fillRect(barX, barY, barMaxWidth, barHeight, ST77XX_BLACK);
  tft.drawRect(barX, barY, barMaxWidth, barHeight, ST77XX_WHITE);
  tft.fillRect(barX, barY, barWidth, barHeight, ST77XX_GREEN);
}

void displayNoContent() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_RED);
  tft.setTextSize(1);
  tft.setCursor(5, 30);
  tft.println("nothing is playing");
  lastSong = "";
}

void downloadAndDisplayAlbumArt(String imageURL) {
  HTTPClient http;
  http.begin(imageURL);
  int httpResponseCode = http.GET();

  if (httpResponseCode == 200 && http.getSize() > 0) {
    WiFiClient* stream = http.getStreamPtr();
    File file = SPIFFS.open("/album.jpg", FILE_WRITE);
    if (!file) return;

    uint8_t buffer[512];
    while (stream->available()) {
      int len = stream->read(buffer, sizeof(buffer));
      file.write(buffer, len);
    }
    file.close();
    decodeAndDrawImage("/album.jpg");
  }
  http.end();
}

void decodeAndDrawImage(const char *filename) {
  File file = SPIFFS.open(filename, FILE_READ);
  if (!file) return;

  if (!JpegDec.decodeFsFile(file)) {
    file.close();
    return;
  }

  float scale = 0.8;
  int scaledHeight = JpegDec.height * scale;
  int yOffset = (scaledHeight < 105) ? (105 - scaledHeight) / 2 : 0;
  int xOffset = (tft.width() - (JpegDec.width * scale)) / 2;
  jpegRenderScaledNearest(xOffset, yOffset, scale);
  file.close();
}

void jpegRenderScaledNearest(int xpos, int ypos, float scale) {
  const int maxArtHeight = 100;
  while (JpegDec.read()) {
    uint16_t *pImg = JpegDec.pImage;
    int mcuX = JpegDec.MCUx * JpegDec.MCUWidth;
    int mcuY = JpegDec.MCUy * JpegDec.MCUHeight;

    for (int y = 0; y < JpegDec.MCUHeight; y++) {
      for (int x = 0; x < JpegDec.MCUWidth; x++) {
        int sx = xpos + (mcuX + x) * scale;
        int sy = ypos + (mcuY + y) * scale;
        if (sx < tft.width() && sy < maxArtHeight) {
          tft.drawPixel(sx, sy, pImg[y * JpegDec.MCUWidth + x]);
        }
      }
    }
  }
}

void animateScreenTransition() {
  tft.fillScreen(ST77XX_BLACK);
}

void showStartupScreen() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(2);
  tft.setCursor(10, 30);
  tft.println("welcome");
  tft.setCursor(10, 60);
  tft.setTextSize(1);
  tft.println("launching...");
  tft.setCursor(10, 80);
  tft.println("spotify display");
  tft.fillRect(0, 145, tft.width(), 10, ST77XX_BLACK);
}

void setup() {
  Serial.begin(115200);
  spi.begin(18, -1, 19);
  tft.initR(INITR_BLACKTAB);
  tft.fillScreen(ST77XX_BLACK);
  showStartupScreen();

  connectWiFi();
  setupSPIFFS();
  if (!refreshAccessToken()) {
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(5, 60);
    tft.setTextColor(ST77XX_RED);
    tft.setTextSize(1);
    tft.println("Token refresh failed");
    delay(5000);
    ESP.restart();
  }
}

void loop() {
  unsigned long now = millis();

  if (now - lastFetchTime >= 10000) {
    fetchSpotifyData();
    lastFetchTime = now;
  }

  if (lastSong != "" && now - lastProgressUpdate >= 1000 && currentProgress < totalDuration) {
    currentProgress += 1000;
    lastProgressUpdate = now;
    updateProgressBar(currentProgress, totalDuration, 145);
  }

  if (lastSong != "") {
    if (pauseScrolling) {
      if (now - scrollPauseStart > 800) {
        scrollSongX = tft.width();
        scrollArtistX = tft.width();
        pauseScrolling = false;
      }
    } else if (now - lastScrollTime >= 50) {
      lastScrollTime = now;

      if (songPixelWidth > tft.width()) {
        scrollSongX--;
        if (scrollSongX < -songPixelWidth) pauseScrolling = true;
      } else {
        scrollSongX = 5;
      }

      

      if (!pauseScrolling) displayOnTFT();
      else scrollPauseStart = now;
    }
  }
}
