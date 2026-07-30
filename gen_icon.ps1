# Generate a 32x32 ICO file with alpha channel using GDI+ rendering
# Output: app.ico in the project root

Add-Type -AssemblyName System.Drawing

$icoPath = Join-Path $PSScriptRoot "..\app.ico"
$icoPath = [System.IO.Path]::GetFullPath($icoPath)

$size = 32

# --- Render the bitmap with GDI+ (anti-aliased) ---
$bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
$g.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
$g.Clear([System.Drawing.Color]::Transparent)

# Outer circle - deep blue gradient effect
$bgPath = New-Object System.Drawing.Drawing2D.GraphicsPath
$bgPath.AddEllipse(1, 1, 30, 30)
$bgBrush = New-Object System.Drawing.Drawing2D.PathGradientBrush($bgPath)
$bgBrush.CenterColor = [System.Drawing.Color]::FromArgb(70, 110, 200)
$bgBrush.SurroundColors = @([System.Drawing.Color]::FromArgb(35, 60, 130))
$g.FillEllipse($bgBrush, 1, 1, 30, 30)
$bgBrush.Dispose()

# White ring
$ringPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(220, 220, 230), 2)
$g.DrawEllipse($ringPen, 2, 2, 28, 28)
$ringPen.Dispose()

# Draw shield shape in white
$shieldPoints = @(
    (New-Object System.Drawing.PointF(16, 6)),
    (New-Object System.Drawing.PointF(24, 10)),
    (New-Object System.Drawing.PointF(24, 18)),
    (New-Object System.Drawing.PointF(16, 26)),
    (New-Object System.Drawing.PointF(8, 18)),
    (New-Object System.Drawing.PointF(8, 10))
)
$shieldBrush = New-Object System.Drawing.SolidBrush([System.Drawing.Color]::FromArgb(240, 245, 255))
$g.FillPolygon($shieldBrush, $shieldPoints)
$shieldBrush.Dispose()

# Draw checkmark inside shield (light green)
$checkPen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(40, 180, 70), 2.5)
$checkPen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
$checkPen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
$g.DrawLine($checkPen, 13, 16, 15, 19)
$g.DrawLine($checkPen, 15, 19, 20, 12)
$checkPen.Dispose()

$g.Dispose()

# --- Extract raw BGRA pixel data (bottom-up for DIB) ---
$rect = New-Object System.Drawing.Rectangle(0, 0, $size, $size)
$bmpData = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$stride = $bmpData.Stride
$pixelBytes = New-Object byte[] ($stride * $size)
[System.Runtime.InteropServices.Marshal]::Copy($bmpData.Scan0, $pixelBytes, 0, $pixelBytes.Length)
$bmp.UnlockBits($bmpData)
$bmp.Dispose()

# --- Build ICO file ---
$ms = New-Object System.IO.MemoryStream

# ICO header
$header = New-Object byte[] 6
$header[2] = 1  # type = icon
$header[4] = 1  # count = 1 image
$ms.Write($header, 0, 6)

# XOR bitmap (32bpp BGRA, bottom-up)
$xorSize = $size * $size * 4  # 4096
# AND mask (1bpp, rows padded to 4 bytes, bottom-up)
$andRowSize = [Math]::Ceiling($size / 8.0)
$andRowPadded = ($andRowSize + 3) -band (-bnot 3)  # round up to 4
$andSize = $andRowPadded * $size  # 128
$imageDataSize = 40 + $xorSize + $andSize  # 4264

# Directory entry
$dir = New-Object byte[] 16
$dir[0] = $size        # width (0 = 256)
$dir[1] = $size        # height
$dir[2] = 0            # color count
$dir[3] = 0            # reserved
$dir[4] = 1; $dir[5] = 0   # planes = 1
$dir[6] = 32; $dir[7] = 0  # bitcount = 32
# Bytes in resource
[System.BitConverter]::GetBytes([uint32]$imageDataSize).CopyTo($dir, 8)
# Offset to image data
[System.BitConverter]::GetBytes([uint32]22).CopyTo($dir, 12)
$ms.Write($dir, 0, 16)

# BITMAPINFOHEADER
$bmi = New-Object byte[] 40
[System.BitConverter]::GetBytes([uint32]40).CopyTo($bmi, 0)    # biSize
[System.BitConverter]::GetBytes([int32]$size).CopyTo($bmi, 4)  # biWidth
[System.BitConverter]::GetBytes([int32]($size * 2)).CopyTo($bmi, 8)  # biHeight (doubled for XOR+AND)
[System.BitConverter]::GetBytes([uint16]1).CopyTo($bmi, 12)    # biPlanes
[System.BitConverter]::GetBytes([uint16]32).CopyTo($bmi, 14)   # biBitCount
[System.BitConverter]::GetBytes([uint32]0).CopyTo($bmi, 16)    # biCompression = BI_RGB
[System.BitConverter]::GetBytes([uint32]($xorSize + $andSize)).CopyTo($bmi, 20)  # biSizeImage
$ms.Write($bmi, 0, 40)

# XOR bitmap data (convert top-down to bottom-up, RGBA -> BGRA)
$xorData = New-Object byte[] $xorSize
for ($y = 0; $y -lt $size; $y++) {
    $srcRow = ($size - 1 - $y) * $stride  # bottom-up
    $dstRow = $y * ($size * 4)
    for ($x = 0; $x -lt $size; $x++) {
        $src = $srcRow + $x * 4
        $dst = $dstRow + $x * 4
        # GDI+ bitmap is BGRA on Windows, which is what we need
        $xorData[$dst]     = $pixelBytes[$src]      # B
        $xorData[$dst + 1] = $pixelBytes[$src + 1]   # G
        $xorData[$dst + 2] = $pixelBytes[$src + 2]   # R
        $xorData[$dst + 3] = $pixelBytes[$src + 3]   # A
    }
}
$ms.Write($xorData, 0, $xorSize)

# AND mask (1bpp, all zeros = fully opaque for 32-bit icons)
$andData = New-Object byte[] $andSize  # all zeros
$ms.Write($andData, 0, $andSize)

# Write to file
[System.IO.File]::WriteAllBytes($icoPath, $ms.ToArray())
$ms.Dispose()

Write-Host "ICO file generated: $icoPath ($((Get-Item $icoPath).Length) bytes)"
