# Decode an image through Windows Imaging Component (e.g. Nikon NEF Codec)
# and write the first frame as raw little-endian Rgb48 (or whatever format the
# codec returns) to an output file, printing size and pixel format.
# usage: powershell -File wicdecode.ps1 in.NEF out.rgb48
param([string]$In, [string]$Out)
Add-Type -AssemblyName PresentationCore
$s = [IO.File]::OpenRead($In)
try {
    $d = [Windows.Media.Imaging.BitmapDecoder]::Create($s,
        [Windows.Media.Imaging.BitmapCreateOptions]::PreservePixelFormat,
        [Windows.Media.Imaging.BitmapCacheOption]::OnLoad)
    $fr = $d.Frames[0]
    $bpp = $fr.Format.BitsPerPixel
    $stride = [int](($fr.PixelWidth * $bpp + 7) / 8)
    $buf = New-Object byte[] ($stride * $fr.PixelHeight)
    $fr.CopyPixels($buf, $stride, 0)
    [IO.File]::WriteAllBytes($Out, $buf)
    "{0} {1} {2} {3}" -f $fr.PixelWidth, $fr.PixelHeight, $fr.Format, $d.CodecInfo.FriendlyName
} finally { $s.Close() }
