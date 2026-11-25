# Converts the code-owned SVG primitives into a Windows multi-resolution ICO.
# Run after editing controller.svg; no external image tools are required.
param([string]$PreviewDirectory='')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Drawing
[xml]$taskSvg=Get-Content -Raw -Encoding UTF8 -LiteralPath (Join-Path $PSScriptRoot 'controller.svg')
function Get-Color([string]$Value){[Drawing.ColorTranslator]::FromHtml($Value)}
function Draw-Icon([int]$Size){
    $taskLarge=[Drawing.Bitmap]::new($Size*4,$Size*4)
    $taskGraphics=[Drawing.Graphics]::FromImage($taskLarge)
    try {
        $taskGraphics.Clear([Drawing.Color]::Transparent)
        $taskGraphics.SmoothingMode=[Drawing.Drawing2D.SmoothingMode]::AntiAlias
        $taskGraphics.ScaleTransform([single]($Size*4/256),[single]($Size*4/256))
        foreach($taskNode in $taskSvg.DocumentElement.ChildNodes){
            switch($taskNode.LocalName){
                'rect' {
                    $taskX=[single]$taskNode.x;$taskY=[single]$taskNode.y
                    $taskW=[single]$taskNode.width;$taskH=[single]$taskNode.height;$taskD=2*[single]$taskNode.rx
                    $taskPath=[Drawing.Drawing2D.GraphicsPath]::new()
                    $taskBrush=[Drawing.SolidBrush]::new((Get-Color $taskNode.fill))
                    try {
                        $taskPath.AddArc($taskX,$taskY,$taskD,$taskD,180,90)
                        $taskPath.AddArc($taskX+$taskW-$taskD,$taskY,$taskD,$taskD,270,90)
                        $taskPath.AddArc($taskX+$taskW-$taskD,$taskY+$taskH-$taskD,$taskD,$taskD,0,90)
                        $taskPath.AddArc($taskX,$taskY+$taskH-$taskD,$taskD,$taskD,90,90)
                        $taskPath.CloseFigure();$taskGraphics.FillPath($taskBrush,$taskPath)
                    } finally {$taskPath.Dispose();$taskBrush.Dispose()}
                }
                'line' {
                    $taskPen=[Drawing.Pen]::new((Get-Color $taskNode.stroke),[single]$taskNode.GetAttribute('stroke-width'))
                    try {$taskPen.StartCap=[Drawing.Drawing2D.LineCap]::Round;$taskPen.EndCap=$taskPen.StartCap;$taskGraphics.DrawLine($taskPen,[single]$taskNode.x1,[single]$taskNode.y1,[single]$taskNode.x2,[single]$taskNode.y2)} finally {$taskPen.Dispose()}
                }
                'circle' {
                    $taskR=[single]$taskNode.r;$taskBrush=[Drawing.SolidBrush]::new((Get-Color $taskNode.fill))
                    try {$taskGraphics.FillEllipse($taskBrush,[single]$taskNode.cx-$taskR,[single]$taskNode.cy-$taskR,2*$taskR,2*$taskR)} finally {$taskBrush.Dispose()}
                }
                'title' {}
                default {throw ('Unsupported icon SVG element: '+$taskNode.LocalName)}
            }
        }
        $taskSmall=[Drawing.Bitmap]::new($Size,$Size)
        $taskResize=[Drawing.Graphics]::FromImage($taskSmall)
        try {
            $taskResize.InterpolationMode=[Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $taskResize.CompositingMode=[Drawing.Drawing2D.CompositingMode]::SourceCopy
            $taskResize.PixelOffsetMode=[Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $taskResize.DrawImage($taskLarge,[Drawing.Rectangle]::new(0,0,$Size,$Size),0,0,$taskLarge.Width,$taskLarge.Height,[Drawing.GraphicsUnit]::Pixel)
        } finally {$taskResize.Dispose()}
        return ,$taskSmall
    } finally {$taskGraphics.Dispose();$taskLarge.Dispose()}
}
$taskSizes=@(16,20,24,32,40,48,64,96,128,256)
$taskImages=@()
foreach($taskSize in $taskSizes){
    $taskImage=Draw-Icon $taskSize;$taskMemory=[IO.MemoryStream]::new()
    try {
        $taskImage.Save($taskMemory,[Drawing.Imaging.ImageFormat]::Png)
        $taskImages+=,@($taskMemory.ToArray())
        if($PreviewDirectory){$taskPreview=[IO.Path]::GetFullPath($PreviewDirectory);[IO.Directory]::CreateDirectory($taskPreview)|Out-Null;$taskImage.Save((Join-Path $taskPreview ("controller-$taskSize.png")),[Drawing.Imaging.ImageFormat]::Png)}
    } finally {$taskImage.Dispose();$taskMemory.Dispose()}
}
$taskStream=[IO.MemoryStream]::new();$taskWriter=[IO.BinaryWriter]::new($taskStream)
try {
    $taskWriter.Write([uint16]0);$taskWriter.Write([uint16]1);$taskWriter.Write([uint16]$taskSizes.Count)
    $taskOffset=6+16*$taskSizes.Count
    for($taskI=0;$taskI -lt $taskSizes.Count;++$taskI){
        $taskDimension=if($taskSizes[$taskI] -eq 256){0}else{$taskSizes[$taskI]}
        $taskWriter.Write([byte]$taskDimension);$taskWriter.Write([byte]$taskDimension)
        $taskWriter.Write([byte]0);$taskWriter.Write([byte]0);$taskWriter.Write([uint16]1);$taskWriter.Write([uint16]32)
        $taskWriter.Write([uint32]$taskImages[$taskI].Count);$taskWriter.Write([uint32]$taskOffset)
        $taskOffset+=$taskImages[$taskI].Count
    }
    foreach($taskBytes in $taskImages){$taskWriter.Write([byte[]]$taskBytes)}
    [IO.File]::WriteAllBytes((Join-Path $PSScriptRoot 'controller.ico'),$taskStream.ToArray())
} finally {$taskWriter.Dispose();$taskStream.Dispose()}
Write-Output 'Built controller.ico: 16/20/24/32/40/48/64/96/128/256 px, transparent RGBA.'
