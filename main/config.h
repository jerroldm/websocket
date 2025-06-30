#define WAVESHARE_ESP32_S3_SIM7670G
//#define LILYGO_T_SIM7670G_S3

#if defined(WAVESHARE_ESP32_S3_SIM7670G)

    #define BOARD_NAME              "WAVESHARE_ESP32_S3_SIM7670G"

    #define MODEM_BAUD_RATE         115200
    #define MODEM_DTR_PIN           45
    #define MODEM_TX_PIN            18
    #define MODEM_RX_PIN            17
    #define MODEM_RTS_PIN           -1      // -1 = Disable
    #define MODEM_CTS_PIN           -1      // -1 = Disable
    #define MODEM_PWRKEY_PIN        -1      // -1 = Disable or 4
    #define MODEM_POWER_PIN         -1      // -1 = Disable or 12
    #define MODEM_RESET_PIN         -1      // -1 = Disable or 5
    #define MODEM_RESET_LEVEL        1

#elif defined(LILYGO_T_SIM7670G_S3)

    #define BOARD_NAME                          "LILYGO_T_SIM7670G_S3"

    #define MODEM_BAUD_RATE                     (115200)
    #define MODEM_DTR_PIN                       (9)
    #define MODEM_TX_PIN                        (11)
    #define MODEM_RX_PIN                        (10)
    #define MODEM_RTS_PIN                       -1      // -1 = Disable
    #define MODEM_CTS_PIN                       -1      // -1 = Disable
    // The modem boot pin needs to follow the startup sequence.
    #define MODEM_PWRKEY_PIN                    (18)
    #define BOARD_LED_PIN                       (12)
    #define LED_ON                              (LOW)
    // There is no modem power control, the LED Pin is used as a power indicator here.
    #define MODEM_POWER_PIN                     -1      // -1 = Disable
    #define MODEM_RING_PIN                      (3)
    #define MODEM_RESET_PIN                     (17)
    #define MODEM_RESET_LEVEL                   LOW
    #define SerialAT                            Serial1

    #define BOARD_BAT_ADC_PIN                   (4)
    #define BOARD_SOLAR_ADC_PIN                 (5)
    #define BOARD_MISO_PIN                      (47)
    #define BOARD_MOSI_PIN                      (14)
    #define BOARD_SCK_PIN                       (21)
    #define BOARD_SD_CS_PIN                     (13)

    #ifndef TINY_GSM_MODEM_SIM7672
        #define TINY_GSM_MODEM_SIM7672
    #endif

    #define MODEM_GPS_ENABLE_GPIO               (4)
    #define MODEM_GPS_ENABLE_LEVEL              (1)

#endif
