using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace ZiiuApp.Backend;

public sealed record DisplayInfo(string HardwareId, string FriendlyName, string GdiName, int Width, int Height,
                                 double RefreshHz, int X, int Y)
{
    public bool Primary => X == 0 && Y == 0;
    public override string ToString() =>
        $"{(FriendlyName.Length > 0 ? FriendlyName : "Display")}  ·  {HardwareId}  ·  {GdiName.Replace(@"\\.\", "")}  ·  " +
        $"{Width}×{Height} @ {RefreshHz:0.##} Hz{(Primary ? "  ·  primary" : "")}";
}

// Active displays from the Windows display configuration (QueryDisplayConfig). The stream
// selects a display by its monitor hardware ID (e.g. YOUR_DISPLAY_ID, the dummy plug), which stays
// the same across reboots while \\.\DISPLAYn can change.
public static class DisplayList
{
    public static List<DisplayInfo> Active()
    {
        var list = new List<DisplayInfo>();
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, out uint np, out uint nm) != 0) return list;
        var paths = new PATH_INFO[np];
        var modes = new MODE_INFO[nm];
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, ref np, paths, ref nm, modes, IntPtr.Zero) != 0) return list;
        for (int i = 0; i < np; i++)
        {
            ref PATH_INFO p = ref paths[i];
            var target = new TARGET_DEVICE_NAME();
            target.header.type = 2;   // DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME
            target.header.size = (uint)Marshal.SizeOf<TARGET_DEVICE_NAME>();
            target.header.adapterId = p.targetInfo.adapterId;
            target.header.id = p.targetInfo.id;
            if (DisplayConfigGetDeviceInfo(ref target) != 0) continue;
            var source = new SOURCE_DEVICE_NAME();
            source.header.type = 1;   // DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME
            source.header.size = (uint)Marshal.SizeOf<SOURCE_DEVICE_NAME>();
            source.header.adapterId = p.sourceInfo.adapterId;
            source.header.id = p.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(ref source) != 0) continue;

            // \\?\DISPLAY#YOUR_DISPLAY_ID#5&...#{guid}: the hardware ID is the second part.
            string[] parts = (target.monitorDevicePath ?? "").Split('#');
            string hwid = parts.Length > 1 ? parts[1] : "";
            int w = 0, h = 0, x = 0, y = 0;
            uint mi = p.sourceInfo.modeInfoIdx;
            if (mi < nm && modes[mi].infoType == 1)   // DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE
            {
                w = (int)modes[mi].width; h = (int)modes[mi].height;
                x = modes[mi].posX; y = modes[mi].posY;
            }
            var r = p.targetInfo.refreshRate;
            double hz = r.Denominator != 0 ? (double)r.Numerator / r.Denominator : 0;
            if (hwid.Length > 0)
                list.Add(new DisplayInfo(hwid, target.monitorFriendlyDeviceName ?? "", source.viewGdiDeviceName ?? "", w, h, hz, x, y));
        }
        return list;
    }

    const uint QDC_ONLY_ACTIVE_PATHS = 2;

    [StructLayout(LayoutKind.Sequential)] struct LUID { public uint Low; public int High; }
    [StructLayout(LayoutKind.Sequential)] struct RATIONAL { public uint Numerator, Denominator; }

    [StructLayout(LayoutKind.Sequential)]
    struct PATH_SOURCE_INFO { public LUID adapterId; public uint id, modeInfoIdx, statusFlags; }

    [StructLayout(LayoutKind.Sequential)]
    struct PATH_TARGET_INFO
    {
        public LUID adapterId;
        public uint id, modeInfoIdx, outputTechnology, rotation, scaling;
        public RATIONAL refreshRate;
        public uint scanLineOrdering;
        public int targetAvailable;
        public uint statusFlags;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct PATH_INFO { public PATH_SOURCE_INFO sourceInfo; public PATH_TARGET_INFO targetInfo; public uint flags; }

    // DISPLAYCONFIG_MODE_INFO: 16-byte header + 48-byte union; only the source mode is read.
    [StructLayout(LayoutKind.Explicit, Size = 64)]
    struct MODE_INFO
    {
        [FieldOffset(0)] public uint infoType;
        [FieldOffset(4)] public uint id;
        [FieldOffset(8)] public LUID adapterId;
        [FieldOffset(16)] public uint width;
        [FieldOffset(20)] public uint height;
        [FieldOffset(24)] public uint pixelFormat;
        [FieldOffset(28)] public int posX;
        [FieldOffset(32)] public int posY;
    }

    [StructLayout(LayoutKind.Sequential)]
    struct DEVICE_INFO_HEADER { public uint type, size; public LUID adapterId; public uint id; }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct TARGET_DEVICE_NAME
    {
        public DEVICE_INFO_HEADER header;
        public uint flags, outputTechnology;
        public ushort edidManufactureId, edidProductCodeId;
        public uint connectorInstance;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string monitorFriendlyDeviceName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string monitorDevicePath;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct SOURCE_DEVICE_NAME
    {
        public DEVICE_INFO_HEADER header;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string viewGdiDeviceName;
    }

    [DllImport("user32.dll")]
    static extern int GetDisplayConfigBufferSizes(uint flags, out uint numPaths, out uint numModes);

    [DllImport("user32.dll")]
    static extern int QueryDisplayConfig(uint flags, ref uint numPaths, [Out] PATH_INFO[] paths, ref uint numModes,
                                         [Out] MODE_INFO[] modes, IntPtr topologyId);

    [DllImport("user32.dll")]
    static extern int DisplayConfigGetDeviceInfo(ref TARGET_DEVICE_NAME request);

    [DllImport("user32.dll")]
    static extern int DisplayConfigGetDeviceInfo(ref SOURCE_DEVICE_NAME request);
}
