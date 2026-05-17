#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <vector>

// ===== CONFIG =====
#define WIFI_SSID "BALIBAG"
#define WIFI_PASS "Balibag_0021"
#define DAC_PIN 25
#define LOUDNESS_PIN 34
#define AUDIO_SERVER "http://10.33.51.106:8000"
#define SUB_HUB_IP "10.33.51.179"

// ===== GLOBALS =====
AsyncWebServer server(80);
TaskHandle_t audioTaskHandle = NULL;
bool isPlaying = false;
String currentAudio = "";
std::vector<String> playlist;

// Sub Hub states
bool subLeftPlaying = false;
bool subRightPlaying = false;
String lastTrackPlayed = ""; 
String subLeftCurrent = "";
String subRightCurrent = "";

// Active Hub: 0=Main, 1=Sub Left, 2=Sub Right
int activeHub = 0;

// Auto mode
bool autoMode = false;
float loudnessThreshold = 50.0;

// ===== FUNCTION: SEND COMMAND TO SUB HUB =====
void sendSubHubCommand(const String &channel, const String &fileName) {
    HTTPClient http;
    String url = "http://" + String(SUB_HUB_IP) + "/subhub_command";
    String body = String("{\"action\":\"") + String(fileName != "" ? "play" : "stop") + 
              String("\",\"file\":\"") + fileName + 
              String("\",\"channel\":\"") + channel + "\"}";

    http.begin(url);
    http.addHeader("Content-Type","application/json");
    int httpCode = http.POST(body);
    if(httpCode > 0){
        String response = http.getString();
        Serial.printf("✅ Sub Hub command sent: %s | Channel: %s | Response: %s\n",
                      fileName.c_str(), channel.c_str(), response.c_str());

        // Update Sub Hub state
        if(channel == "left"){
            if(fileName != "" && response.startsWith("Playing")) subLeftPlaying = true;
            else subLeftPlaying = false;
            subLeftCurrent = fileName;
        }
        else if(channel == "right"){
            if(fileName != "" && response.startsWith("Playing")) subRightPlaying = true;
            else subRightPlaying = false;
            subRightCurrent = fileName;
        }
    } else {
        Serial.printf("❌ Failed to send Sub Hub command | Error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
}

// ===== FETCH PLAYLIST =====
bool fetchPlaylist() {
    playlist.clear();
    HTTPClient http;
    String url = String(AUDIO_SERVER) + "/list";
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode != 200) { 
        Serial.printf("❌ Playlist fetch failed: %d\n", httpCode); 
        http.end(); 
        return false; 
    }

    String payload = http.getString();
    http.end();

    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, payload)) { 
        Serial.println("❌ JSON parse failed"); 
        return false; 
    }

    JsonArray files = doc["files"];
    for (JsonVariant v : files) playlist.push_back(v.as<String>());

    Serial.printf("✅ Playlist fetched (%d files)\n", playlist.size());
    return true;
}

// ===== AUDIO STREAM TASK (Main Hub) =====
void startAudioTask(String fileName) {
    if (audioTaskHandle != NULL) return;
    currentAudio = fileName;
    isPlaying = true;

    xTaskCreate([](void*){
        while(isPlaying && currentAudio != "") {
            String url = String(AUDIO_SERVER) + "/audio/" + currentAudio;
            HTTPClient http;
            http.begin(url);
            int httpCode = http.GET();
            if (httpCode != 200) { Serial.printf("❌ Audio fetch failed: %d\n", httpCode); http.end(); break; }

            WiFiClient *stream = http.getStreamPtr();
            const int bufSize = 1024;
            uint8_t buf[bufSize];

            Serial.printf("🎵 Playing %s on Main Hub\n", currentAudio.c_str());

            while(isPlaying && stream->connected()){
                int len = stream->available();
                if(len>0){
                    if(len>bufSize) len=bufSize;
                    int c = stream->readBytes(buf,len);
                    for(int i=0;i<c;i++){
                        dacWrite(DAC_PIN, buf[i]);
                        delayMicroseconds(125); // 8kHz PCM
                    }
                }
                vTaskDelay(1);
            }

            http.end();
            if(!isPlaying) break;
        }

        isPlaying = false;
        audioTaskHandle = NULL;
        vTaskDelete(NULL);
    },"AudioTask",8192,NULL,1,&audioTaskHandle);
}

void stopAudioTask() {
    if(audioTaskHandle != NULL){
        isPlaying = false;
        vTaskDelay(10);
        audioTaskHandle = NULL;
    }
}

// ===== WEB SERVER =====
void initWebServer() {
    server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

    // ===== STATUS =====
    server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request){
        int rawLoudness = analogRead(LOUDNESS_PIN);
        float loudPercent = (rawLoudness / 4095.0) * 100.0;

        StaticJsonDocument<256> doc;
        doc["activeHub"] = activeHub;
        doc["currentAudio"] = currentAudio;
        doc["autoMode"] = autoMode;
        doc["subLeft"] = true;
        doc["subRight"] = true;
        doc["loudness"] = loudPercent;
        String json; serializeJson(doc,json);
        request->send(200,"application/json",json);
    });

    // ===== AUDIO LIST =====
    server.on("/audio-list", HTTP_GET, [](AsyncWebServerRequest *request){
        StaticJsonDocument<512> doc;
        JsonArray arr = doc.to<JsonArray>();
        for(auto &f:playlist) arr.add(f);
        String json; serializeJson(doc,json);
        request->send(200,"application/json",json);
    });

    // ===== CONTROL =====
    server.on("/control", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!request->hasParam("action", true)) { 
            request->send(400,"text/plain","Missing action"); 
            return; 
        }

        String action = request->getParam("action", true)->value();
        String fileName = "";
        if(action=="play_pause" && request->hasParam("file", true))
            fileName = request->getParam("file", true)->value();


        // ===== SWITCH HUB =====
        if(action == "switch_hub"){
            int prevHub = activeHub;
            activeHub = (activeHub + 1) % 3;
            Serial.printf("🔁 Active Hub switched to: %s\n",
                          (activeHub==0 ? "Main" : activeHub==1 ? "Sub Left" : "Sub Right"));

            // Stop audio on previous hub
            if(prevHub == 0) stopAudioTask();
            else if(prevHub == 1) sendSubHubCommand("left","");
            else if(prevHub == 2) sendSubHubCommand("right","");

            request->send(200,"text/plain","OK");
            return;
        }


        // ===== PLAY/PAUSE =====
        if(action == "play_pause"){
            // Decide hub at the moment of action
            int hubToUse = activeHub; // default manual
            if(autoMode){
                int rawLoudness = analogRead(LOUDNESS_PIN);
                float loudPercent = (rawLoudness / 4095.0) * 100.0;
                if(loudPercent <= 20.0) hubToUse = 0;        // Main
                else if(loudPercent <= 50.0) hubToUse = 1;   // Sub Left
                else hubToUse = 2;                            // Sub Right
            }

            // Stop previous hub if switching
            if(hubToUse != activeHub){
                if(activeHub == 0) stopAudioTask();
                else if(activeHub == 1) sendSubHubCommand("left","");
                else if(activeHub == 2) sendSubHubCommand("right","");
            }

            // Play/pause on the chosen hub
            if(hubToUse == 0){ // Main Hub
                if(currentAudio == fileName && isPlaying){
                    stopAudioTask(); // pause
                } else {
                    currentAudio = fileName;
                    startAudioTask(currentAudio);
                    lastTrackPlayed = fileName; // ✅ track for next/prev
                }
            } else { // Sub Hub
                String channel = (hubToUse == 1) ? "left" : "right";
                bool &isPlayingSub = (channel == "left") ? subLeftPlaying : subRightPlaying;
                String &currentSub = (channel == "left") ? subLeftCurrent : subRightCurrent;

                if(currentSub == fileName && isPlayingSub){
                    sendSubHubCommand(channel,""); // pause
                    isPlayingSub = false;
                } else {
                    sendSubHubCommand(channel,fileName); // play
                    isPlayingSub = true;
                    currentSub = fileName;
                    lastTrackPlayed = fileName; // ✅ track for next/prev
                }

                currentAudio = ""; // main hub audio separate
            }

            // Lock activeHub for this playback
            activeHub = hubToUse;
        }



        // ===== NEXT/PREV =====
        else if(action=="next" || action=="prev"){
            if(playlist.size() == 0){ request->send(200,"text/plain","OK"); return; }

            // Use lastTrackPlayed instead of currentAudio
            int idx = -1;
            for(size_t i=0;i<playlist.size();i++)
                if(playlist[i] == lastTrackPlayed) idx = i;

            // Calculate next/prev
            if(action=="next") idx = (idx+1) % playlist.size();
            else idx = (idx <= 0 ? playlist.size()-1 : idx-1);

            String nextTrack = playlist[idx];

            // Decide hub at the moment of action
            int hubToUse = activeHub;
            if(autoMode){
                int rawLoudness = analogRead(LOUDNESS_PIN);
                float loudPercent = (rawLoudness / 4095.0) * 100.0;
                if(loudPercent <= 20.0) hubToUse = 0;
                else if(loudPercent <= 50.0) hubToUse = 1;
                else hubToUse = 2;
            }

            // Stop previous hub if switching
            if(hubToUse != activeHub){
                if(activeHub == 0) stopAudioTask();
                else if(activeHub == 1) sendSubHubCommand("left","");
                else if(activeHub == 2) sendSubHubCommand("right","");
            }

            // Play on the decided hub
            if(hubToUse == 0){ // Main Hub
                currentAudio = nextTrack;
                startAudioTask(currentAudio);
                lastTrackPlayed = nextTrack; // ✅ track for next/prev
            } else { // Sub Hub
                String channel = (hubToUse == 1) ? "left" : "right";
                sendSubHubCommand(channel,nextTrack); // play new track

                if(channel == "left"){
                    subLeftPlaying = true;
                    subLeftCurrent = nextTrack;
                } else {
                    subRightPlaying = true;
                    subRightCurrent = nextTrack;
                }

                currentAudio = "";
                lastTrackPlayed = nextTrack; // ✅ track for next/prev
            }

            // Lock activeHub for this playback
            activeHub = hubToUse;
        }




        // ===== AUTO MODE =====
        else if(action == "auto"){
            autoMode = !autoMode;
            Serial.printf("🤖 Auto Mode: %s\n", autoMode ? "ON" : "OFF");
        }

        request->send(200,"text/plain","OK");
    });



    server.begin();
    Serial.println("✅ Web server started");
}

// ===== SETUP =====
void setup() {
    Serial.begin(115200);

    // Connect WiFi
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED){
        Serial.print(".");
        delay(500);
    }
    Serial.println("\n✅ WiFi Connected: " + WiFi.localIP().toString());

    // Mount LittleFS
    if(!LittleFS.begin()){ Serial.println("⚠️ LittleFS Mount Failed"); return; }
    Serial.println("✅ LittleFS mounted");

    // DAC Init
    dacWrite(DAC_PIN, 0);

    // Fetch initial playlist
    fetchPlaylist();

    // Start Web Server
    initWebServer();
}

// ===== LOOP =====
void loop() {
    // Nothing needed for auto mode during playback.
    // Loop can just delay to save CPU.
    delay(500);
}


