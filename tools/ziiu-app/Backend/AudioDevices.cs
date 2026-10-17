using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.Win32;

namespace ZiiuApp.Backend;

// Active Windows playback devices by friendly name, e.g. "Odyssey G80SD (NVIDIA High
// Definition Audio)". ziiu-drc's ZIIU_AUDIO matches a case-insensitive part of that name
// (audio_capture.h), so the full name selects exactly one device. Read from the registry
// (the same data as the MMDevice API), so no COM interop is needed for a list.
public static class AudioDevices
{
    const string RenderKey = @"SOFTWARE\Microsoft\Windows\CurrentVersion\MMDevices\Audio\Render";
    const string DeviceDesc = "{a45c254e-df1c-4efd-8020-67d146a850e0},2";      // PKEY_Device_DeviceDesc
    const string InterfaceName = "{b3f8fa53-0004-438e-9003-51a46e139bfc},6";   // PKEY_DeviceInterface_FriendlyName

    public static List<string> ActiveOutputs()
    {
        var names = new List<string>();
        try
        {
            using RegistryKey? render = Registry.LocalMachine.OpenSubKey(RenderKey);
            if (render == null) return names;
            foreach (string id in render.GetSubKeyNames())
            {
                using RegistryKey? dev = render.OpenSubKey(id);
                if (dev?.GetValue("DeviceState") is not int state || state != 1) continue;   // DEVICE_STATE_ACTIVE
                using RegistryKey? props = dev.OpenSubKey("Properties");
                string? desc = props?.GetValue(DeviceDesc) as string;
                string? iface = props?.GetValue(InterfaceName) as string;
                if (string.IsNullOrEmpty(desc)) continue;
                names.Add(string.IsNullOrEmpty(iface) ? desc : $"{desc} ({iface})");
            }
        }
        catch (Exception) { }
        return names.Distinct().OrderBy(n => n, StringComparer.OrdinalIgnoreCase).ToList();
    }
}
