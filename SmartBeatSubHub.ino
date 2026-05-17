#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <vector>

// ===== CONFIG =====
#define WIFI_SSID "BALIBAG"
#define WIFI_PASS "Balibag_0021"
#define DAC_LEFT 25
#define DAC_RIGHT 26
#define AUDIO_SERVER "http://10.33.51.106:8000" // same as main hub

// ===== GLOBALS =====
AsyncWebServer server(80);

String currentAudio = "";
String currentChannel = "left"; // left or right
bool isPlaying = false;
TaskHandle_t audioTaskHandle = NULL;


// ===== AUDIO STREAM TASK =====
void audioTask(void* parameter){
    while(isPlaying && currentAudio != ""){
        String url = String(AUDIO_SERVER) + "/audio/" + currentAudio;
        HTTPClient http;
        http.begin(url);
        int httpCode = http.GET();
        if(httpCode != 200){
            Serial.printf("❌ Audio fetch failed: %d\n", httpCode);
            http.end();
            break;
        }

        WiFiClient *stream = http.getStreamPtr();
        const int bufSize = 1024;
        uint8_t buf[bufSize];
        int dacPin = (currentChannel == "left") ? DAC_LEFT : DAC_RIGHT;

        Serial.printf("🎵 Playing %s on %s channel\n", currentAudio.c_str(), currentChannel.c_str());

        while(isPlaying && stream->connected()){
            int len = stream->available();
            if(len > 0){
                if(len > bufSize) len = bufSize;
                int c = stream->readBytes(buf, len);
                for(int i=0;i<c;i++){
                    dacWrite(dacPin, buf[i]);
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
}

// ===== START / STOP AUDIO =====
void startAudioTask(String fileName, String channel){
    if(audioTaskHandle != NULL) stopAudioTask();
    currentAudio = fileName;
    currentChannel = channel;
    isPlaying = true;
    xTaskCreate(audioTask, "AudioTask", 8192, NULL, 1, &audioTaskHandle);
}

void stopAudioTask(){
    if(audioTaskHandle != NULL){
        isPlaying = false;
        vTaskDelay(10);
        audioTaskHandle = NULL;
    }
}

// ===== SETUP =====
void setup(){
    Serial.begin(115200);

    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connecting");
    while(WiFi.status() != WL_CONNECTED){ Serial.print("."); delay(500); }
    Serial.println("\n✅ WiFi Connected: "+WiFi.localIP().toString());

    dacWrite(DAC_LEFT, 0);
    dacWrite(DAC_RIGHT, 0);

    // HTTP SERVER
server.on("/subhub_command", HTTP_POST,
[](AsyncWebServerRequest *request){}, 
NULL, 
[](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t){

    String body;
    for(size_t i=0;i<len;i++) body += (char)data[i];

    StaticJsonDocument<256> doc;
    if(deserializeJson(doc, body)){
        request->send(400,"text/plain","Invalid JSON");
        return;
    }

    String action  = doc["action"] | "";
    String file    = doc["file"]   | "";
    String channel = doc["channel"]| "left";

    if(action == "play"){
        // If the requested track is already playing → stop it (pause)
        if(isPlaying && currentAudio == file && currentChannel == channel){
            stopAudioTask();
            request->send(200,"text/plain","Paused " + file);
        } 
        else {
            // Stop any existing playback first
            stopAudioTask();
            startAudioTask(file, channel);
            request->send(200,"text/plain","Playing " + file);
        }
    }
    else if(action == "stop"){
        stopAudioTask();
        request->send(200,"text/plain","Stopped");
    }
    else{
        request->send(400,"text/plain","Unsupported action");
    }
});

    server.begin();
    Serial.println("✅ Sub Hub server started, waiting for commands...");
}

// ===== LOOP =====
void loop(){
    delay(1000); // no other processing needed
}
