#!/bin/bash
# A page whose pictures are the point, served from the host. QEMU's user-mode
# network puts the host at 10.0.2.2, so the guest can reach it without any
# setup on either side.
set -e
D=/tmp/webroot
rm -rf $D && mkdir -p $D

convert -size 320x200 gradient:'#3E9BE0-#0B0D12' \
        -pointsize 28 -fill white -gravity center -annotate 0 'PNG 320x200' \
        $D/a.png
convert -size 640x400 plasma:fractal -quality 85 $D/b.jpg
convert -size 120x120 radial-gradient:'#5FBF7F-#141922' $D/small.png
convert -size 1600x900 gradient:'#E0644A-#16283A' \
        -pointsize 90 -fill white -gravity center -annotate 0 'WIDE 1600x900' \
        $D/wide.jpg

cat > $D/index.html <<'EOF'
<!doctype html>
<html><head><title>Картинки</title></head><body>
<h1>Проверка картинок</h1>
<p>Ниже четыре изображения: PNG с градиентом, фотография JPEG,
маленький значок в строке текста и снимок шире окна.</p>

<h3>PNG, 320&times;200</h3>
<img src="a.png" alt="gradient">

<h3>JPEG, 640&times;400</h3>
<img src="b.jpg" alt="plasma">

<h3>Маленькая, в потоке текста</h3>
<p>Слева <img src="small.png" alt="dot" width="24" height="24"> значок должен
стоять прямо в строке, не разрывая её, а текст продолжаться дальше как ни в
чём не бывало и переноситься по ширине окна.</p>

<h3>Шире окна &mdash; должна ужаться</h3>
<img src="wide.jpg" alt="wide">

<h3>Битая ссылка &mdash; должна показать рамку</h3>
<img src="nope.png" alt="missing">

<hr>
<p><a href="/index.html">та же страница ещё раз</a></p>
</body></html>
EOF

echo "wrote $D"
ls -la $D
