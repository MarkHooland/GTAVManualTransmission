/*
 * Massive thanks to t-mat on GitHub for having a simple C++ DInput
 * application! https://gist.github.com/t-mat/1391291
 */

#pragma once
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>

struct DIDevice {
    DIDEVICEINSTANCE diDeviceInstance;
    DIDEVCAPS diDevCaps;
    LPDIRECTINPUTDEVICE8 diDevice;
    DIJOYSTATE2 joystate;
};

class DIDeviceFactory {
public:
    static DIDeviceFactory& Get();

    void Enumerate(LPDIRECTINPUT di,
                   DWORD dwDevType = DI8DEVCLASS_GAMECTRL,
                   LPCDIDATAFORMAT lpdf = &c_dfDIJoystick2,
                   DWORD dwFlags = DIEDFL_ATTACHEDONLY,
                   int maxEntry = 16);
    int GetEntryCount() const;
    const DIDevice* GetEntry(int index) const;
    void Update() const;

protected:
    DIDeviceFactory();
    ~DIDeviceFactory();
    static BOOL CALLBACK DIEnumDevicesCallback_static(LPCDIDEVICEINSTANCE lpddi, LPVOID pvRef);
    BOOL DIEnumDevicesCallback(LPCDIDEVICEINSTANCE lpddi, LPVOID pvRef);
    void clear();

    DIDevice* entry;
    int maxEntry;
    int nEntry;
    LPDIRECTINPUT di;
    LPCDIDATAFORMAT lpdf;
};
