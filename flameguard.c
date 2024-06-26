#include <wiringPi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <dirent.h>
#include <string.h>

#define PCF8591_ADDRESS 0x48

// Define GPIO pins
#define FLAME_SENSOR_PIN 5
#define PIR_SENSOR_PIN   13
#define RELAY_MOTOR_PIN  19
#define RELAY_PUMP_PIN   26
#define BUZZER_PIN       21

#define DS18B20_SENSOR_PATH "/sys/bus/w1/devices/"
#define IMAGE_PATH "/home/pi/captured_image.jpg"  

const char *account_sid = "ACbc4bsdfhgst5styre5a3";
const char *auth_token = "473c9114sgrg3453gfsdgst3f3f364";
const char *from_phone_number = "+12562977125";
const char *to_phone_number = "+14382473887";
const char *address = "124 St Lumen Colorado Canada J9K 3G8";

// Function to read temperature sensor (DS18B20)
float read_ds18b20_temp() {
    DIR *dir;
    struct dirent *dirent;
    char dev[16] = {0};
    char devPath[128] = {0};
    char buf[256] = {0};
    char tmpData[6] = {0};
    ssize_t numRead;
    float temperature = -1;

    dir = opendir(DS18B20_SENSOR_PATH);
    if (dir != NULL) {
        while ((dirent = readdir(dir))) {
            if (dirent->d_type == DT_LNK && strstr(dirent->d_name, "28-")) {
                strcpy(dev, dirent->d_name);
                break;
            }
        }
        closedir(dir);
    } else {
        perror("Couldn't open the w1 devices directory");
        return -1;
    }

    if (strlen(dev) == 0) {
        printf("No DS18B20 sensor found.\n");
        return -1;
    }

    snprintf(devPath, sizeof(devPath), "%s%s/w1_slave", DS18B20_SENSOR_PATH, dev);
    int fd = open(devPath, O_RDONLY);
    if (fd == -1) {
        perror("Couldn't open the w1 device.");
        return -1;
    }

    numRead = read(fd, buf, sizeof(buf));
    close(fd);
    if (numRead > 0 && strstr(buf, "YES")) {
        char *tempStr = strstr(buf, "t=");
        if (tempStr != NULL) {
            strncpy(tmpData, tempStr + 2, 5);
            tmpData[5] = '\0';
            temperature = strtof(tmpData, NULL) / 1000;
        }
    } else {
        printf("Invalid data read from sensor.\n");
        return -1;
    }

    return temperature;
}

// Function to read smoke sensor connected to PCF8591 ADC
int read_smoke_sensor(int fd) {
    int command = 0x41;
    if (write(fd, &command, 1) != 1) {
        printf("Failed to write to the i2c bus.\n");
        return -1;
    }

    if (read(fd, &command, 1) != 1) {
        printf("Failed to read from the i2c bus.\n");
        return -1;
    }

    if (read(fd, &command, 1) != 1) {
        printf("Failed to read from the i2c bus.\n");
        return -1;
    }
    return command;
}

void activate_relay(int pin) {
    digitalWrite(pin, HIGH);
}

void deactivate_relay(int pin) {
    digitalWrite(pin, LOW);
}

void send_emergency_signal() {
    printf("Sending emergency signal to the nearest safety department...\n");
}

void capture_image() {
    system("libcamera-still -o " IMAGE_PATH);
}

void send_sms_message(const char *alert_type, float temperature, int smoke_level, int motion_detected) {
    char body[512];
    snprintf(body, sizeof(body),
             "Emergency Alert!\n"
             "Type: %s\n"
             "Address: %s\n"
             "Temperature: %.2f°C\n"
             "Smoke Level: %d\n"
             "Motion Detected: %d",
             alert_type, address, temperature, smoke_level, motion_detected);

    // Upload the image to a file hosting service (here using imgur as an example)
    char upload_command[2048];
    snprintf(upload_command, sizeof(upload_command),
             "curl -s -F 'image=@%s' -H 'Authorization: Client-ID c092c79891b9605' https://api.imgur.com/3/image | jq -r '.data.link'", IMAGE_PATH);

    printf("Upload command: %s\n", upload_command);

    FILE *upload_pipe = popen(upload_command, "r");
    if (!upload_pipe) {
        printf("Failed to execute upload command.\n");
        return;
    }

    char asset_url[512];
    if (fgets(asset_url, sizeof(asset_url), upload_pipe) == NULL) {
        printf("Failed to read asset URL.\n");
        pclose(upload_pipe);
        return;
    }
    pclose(upload_pipe);

    asset_url[strcspn(asset_url, "\n")] = '\0';

    // Construct the SMS message with the uploaded media URL
    char command[4096];  // Increased buffer size
    snprintf(command, sizeof(command),
             "curl -X POST 'https://api.twilio.com/2010-04-01/Accounts/%s/Messages.json' "
             "--data-urlencode 'To=%s' "
             "--data-urlencode 'From=%s' "
             "--data-urlencode 'Body=%s\nImage URL: %s' "
             "-u %s:%s",
             account_sid, to_phone_number, from_phone_number, body, asset_url, account_sid, auth_token);

    printf("SMS command: %s\n", command);

    system(command);
}

int main(void) {
    int fd;
    char *fileName = "/dev/i2c-1";
    if ((fd = open(fileName, O_RDWR)) < 0) {
        printf("Failed to open the i2c bus.\n");
        return 1;
    }
    if (ioctl(fd, I2C_SLAVE, PCF8591_ADDRESS) < 0) {
        printf("Failed to acquire bus access and/or talk to slave.\n");
        return 1;
    }

    if (wiringPiSetupGpio() == -1) {
        printf("wiringPi setup failed\n");
        return 1;
    }

    // Set sensor pins as input
    pinMode(FLAME_SENSOR_PIN, INPUT);
    pinMode(PIR_SENSOR_PIN, INPUT);

    // Set relay pins as output
    pinMode(RELAY_MOTOR_PIN, OUTPUT);
    pinMode(RELAY_PUMP_PIN, OUTPUT);
    pinMode(BUZZER_PIN, OUTPUT);

    float temperature = 0;
    float previous_temperature = 0;
    int previous_flame_detected = 0;
    int previous_motion_detected = 0;
    int previous_smoke_level = 0;

    while (1) {
        int flame_detected = digitalRead(FLAME_SENSOR_PIN);
        int motion_detected = digitalRead(PIR_SENSOR_PIN);
        int smoke_level = read_smoke_sensor(fd);

        temperature = read_ds18b20_temp();
        if (temperature == -1) {
            printf("Failed to read temperature\n");
        } else if (temperature < -50 || temperature > 150) {
            printf("Read an invalid temperature: %.2f C\n", temperature);
            temperature = previous_temperature; // Use the last valid temperature
        } else {
            printf("Temperature = %.2f C\n", temperature);
            previous_temperature = temperature; // Update the last valid temperature
        }

        printf("Flame: %d, Smoke Level: %d, Motion: %d\n", flame_detected, smoke_level, motion_detected);

        if ((flame_detected && !previous_flame_detected) || (temperature > 50)) {
            printf("Flame detected! Activating safety protocols.\n");
            digitalWrite(BUZZER_PIN, HIGH);   
            activate_relay(RELAY_PUMP_PIN);
            if (motion_detected) {
                activate_relay(RELAY_MOTOR_PIN);
            }   
            send_emergency_signal();
            capture_image();
            send_sms_message("Fire Alert", temperature, smoke_level, motion_detected);
        } else if ((smoke_level > 128 && smoke_level != previous_smoke_level) || (temperature > 35)) {
            printf("Smoke detected! Activating safety protocols.\n");
            digitalWrite(BUZZER_PIN, HIGH); 
            activate_relay(RELAY_MOTOR_PIN);
            send_emergency_signal();
            capture_image();   
            send_sms_message("Smoke Alert", temperature, smoke_level, motion_detected);
        } else if (motion_detected && !previous_motion_detected) {
            printf("Motion detected! Possible theft.\n");
            digitalWrite(BUZZER_PIN, HIGH);
            send_emergency_signal();
            capture_image();  
            send_sms_message("Theft Alert", temperature, smoke_level, motion_detected);
        } else {
            digitalWrite(BUZZER_PIN, LOW); 
            deactivate_relay(RELAY_MOTOR_PIN);  
            deactivate_relay(RELAY_PUMP_PIN);  
        }

        previous_flame_detected = flame_detected;
        previous_motion_detected = motion_detected;
        previous_smoke_level = smoke_level;

        delay(1000);  
    }

    close(fd);
    return 0;
}
