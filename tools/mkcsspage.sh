#!/bin/bash
# A page whose styling is the point.
D=/tmp/webroot
mkdir -p $D

cat > $D/sheet.css <<'EOF'
/* an external stylesheet, fetched separately */
.ads        { display: none; }
#cookiebar  { display: none; }
.note       { color: #2E7D46; }
.warn       { color: #C0392B; font-weight: bold; }
h2          { font-size: 1.8em; }
.small      { font-size: small; }
nav a       { display: none; }   /* has a combinator: must be refused */
EOF

cat > $D/css.html <<'EOF'
<!doctype html>
<html><head><title>CSS</title>
<link rel="stylesheet" href="sheet.css">
<style>
  .hidden   { display: none }
  .quiet    { color: rgb(120,120,130) }
  .shout    { font-weight: 700; font-size: 2em }
  .midnight { color: #050505 }        /* unreadable on a dark ground */
</style>
</head><body>

<h1>Оформление</h1>

<p class="ads">РЕКЛАМА: этой строки быть не должно.</p>
<div id="cookiebar">Мы используем печенье. Этой строки тоже быть не должно.</div>
<p class="hidden">И этой.</p>
<p style="display:none">И этой, из атрибута style.</p>

<h2>Заголовок из внешнего файла (1.8em)</h2>

<p class="note">Зелёная строка — цвет из внешнего файла.</p>
<p class="warn">Красная и жирная.</p>
<p class="quiet">Приглушённый серый через rgb().</p>
<p class="shout">Крупно и жирно.</p>
<p class="small">Мелкая строка.</p>
<p class="midnight">Почти чёрный на тёмном фоне — цвет должен быть отвергнут,
иначе строку не прочитать.</p>

<p style="color:#1A56CC">Синий прямо из атрибута style.</p>

<nav><p><a href="/index.html">ссылка внутри nav</a> — правило "nav a" с
комбинатором должно быть отвергнуто, и ссылка остаётся видимой.</p></nav>

<p>Обычный текст для сравнения. <b>Жирный кусок.</b> И дальше обычный.</p>

<p><a href="/index.html">на страницу с картинками</a></p>
</body></html>
EOF
echo "wrote $D/css.html"
