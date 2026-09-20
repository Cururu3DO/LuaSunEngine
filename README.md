# LuaSun

Engine 2D minimalista escrita em um único `main.cpp`. O motor é C++17 sobre
SDL2 e toda a lógica do jogo é escrita em Lua 5.4.

- Janela, renderização, input, texto, imagens e física de caixas (AABB)
- Cache automático de texturas, fontes e texto rasterizado
- API pequena: seis tabelas globais (`window`, `engine`, `draw`, `input`, `body`, `world`)
- Sem build system, sem dependência de framework: um arquivo, um comando

---

## Sumário

1. [Instalação](#1-instalação)
2. [Estrutura do projeto](#2-estrutura-do-projeto)
3. [Primeiro jogo](#3-primeiro-jogo)
4. [Ciclo de vida](#4-ciclo-de-vida)
5. [API — window](#5-api--window)
6. [API — engine](#6-api--engine)
7. [API — draw](#7-api--draw)
8. [API — input](#8-api--input)
9. [API — body](#9-api--body)
10. [API — world](#10-api--world)
11. [Como a física funciona](#11-como-a-física-funciona)
12. [Desempenho](#12-desempenho)
13. [Erros e depuração](#13-erros-e-depuração)
14. [Limitações conhecidas](#14-limitações-conhecidas)

---

## 1. Instalação

### Automático

```bash
chmod +x install.sh
./install.sh
```

O script detecta o sistema (Debian/Ubuntu, Arch, Fedora, openSUSE, Alpine ou
Termux), instala SDL2, SDL2_image, SDL2_ttf, Lua 5.4 e o compilador, e já
compila o binário.

| Comando | O que faz |
|---|---|
| `./install.sh` | instala tudo e compila |
| `./install.sh --deps` | só instala as bibliotecas |
| `./install.sh --build` | só compila |
| `./install.sh --run` | compila e roda `game/main.lua` |
| `./install.sh --clean` | apaga o binário e a pasta `build/` |

### Manual

```bash
# Debian / Ubuntu
sudo apt install build-essential liblua5.4-dev \
     libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev

g++ -std=c++17 -O2 main.cpp -o luasun \
    -llua5.4 -lSDL2 -lSDL2_image -lSDL2_ttf
```

No Arch e no Termux o header do Lua não fica dentro de uma pasta `lua5.4/`.
O `install.sh` resolve isso gerando uma pasta de compatibilidade em
`build/compat`; se compilar na mão, acrescente `-Ibuild/compat`.

---

## 2. Estrutura do projeto

```
projeto/
├── main.cpp          o motor
├── install.sh        instalador / build
├── luasun            binário gerado
└── game/
    ├── main.lua      ponto de entrada padrão
    └── assets/       imagens e fontes
```

Rodar:

```bash
./luasun                 # carrega game/main.lua
./luasun outro.lua       # carrega outro script
```

Caminhos de imagem e fonte são resolvidos **a partir da pasta de onde você
executou o binário**, não da pasta do script.

---

## 3. Primeiro jogo

```lua
window.create("Meu jogo", 640, 360, { vsync = true })
window.background(18, 18, 26)

local chao   = body.static("chao", 0, 320, 640, 40)
local heroi  = body.dynamic("heroi", 60, 100, 28, 40)

function update(dt)
    heroi.vx = 0
    if input.key("Left")  then heroi.vx = -260 end
    if input.key("Right") then heroi.vx =  260 end

    if input.pressed("Space") and heroi.grounded then
        heroi.vy = -720
    end
end

function render()
    chao:draw()
    heroi:draw(90, 200, 255)
end

engine.run()
```

---

## 4. Ciclo de vida

O script é executado de cima para baixo. Você cria a janela, define as funções
globais que o motor vai chamar e termina com `engine.run()`, que bloqueia até
o jogo fechar.

Três funções globais são opcionais; o motor chama se existirem:

| Função | Quando | Argumentos |
|---|---|---|
| `start()` | uma vez, logo antes do primeiro frame | — |
| `update(dt)` | todo frame, antes da física | `dt` em segundos |
| `render()` | todo frame, depois da física | — |

Ordem interna de cada frame:

```
eventos SDL → estado do teclado/mouse → update(dt) → física → limpa tela → render() → present
```

Isso importa: quando `render()` roda, a física já moveu os corpos, então o que
você desenha é sempre a posição final do frame. E como `update()` roda antes da
física, é ali que você mexe em `vx`/`vy`.

> `dt` é limitado a 0,05 s. Se o sistema travar por meio segundo, o jogo anda
> em câmera lenta em vez de teletransportar o personagem através das paredes.

---

## 5. API — window

### `window.create(titulo, largura, altura [, opcoes])`

Cria a janela e inicializa SDL, SDL_image e SDL_ttf. Só pode ser chamada uma
vez. Tabela `opcoes`:

| Campo | Padrão | Efeito |
|---|---|---|
| `vsync` | `true` | sincroniza com o monitor |
| `resizable` | `false` | janela redimensionável |
| `fullscreen` | `false` | tela cheia (modo desktop) |

```lua
window.create("LuaSun", 800, 600, { vsync = true, resizable = true })
```

### `window.title(texto)`
Troca o título depois de criada.

### `window.size()` → `largura, altura`
Tamanho real da área de desenho (útil com `resizable`).

### `window.background(r, g, b)`
Cor com que a tela é limpa todo frame. Padrão: `20, 20, 26`.

---

## 6. API — engine

| Função | Retorno | Descrição |
|---|---|---|
| `engine.run()` | — | inicia o loop; só retorna quando o jogo fecha |
| `engine.quit()` | — | encerra o loop no fim do frame atual |
| `engine.fps()` | número | FPS suavizado |
| `engine.time()` | número | segundos desde o início do loop |
| `engine.targetFps(n)` | — | limita o FPS; `0` desliga o limite |

`targetFps` só faz sentido com `vsync = false`. Com vsync ligado, o monitor já
faz o trabalho.

---

## 7. API — draw

Todas as funções de desenho só valem dentro de `render()`. Os parâmetros de
cor (`r, g, b, a`) são opcionais e vão de 0 a 255; o padrão é branco opaco.

### Formas

```lua
draw.rectangle(x, y, largura, altura [, r, g, b, a])   -- preenchido
draw.outline(x, y, largura, altura [, r, g, b, a])     -- só contorno
draw.circle(cx, cy, raio [, r, g, b, a])
draw.line(x1, y1, x2, y2 [, r, g, b, a])
```

### Imagens

```lua
draw.image(caminho, x, y [, largura, altura, angulo, flipX, flipY])
```

Sem largura/altura usa o tamanho original. `angulo` em graus, `flipX`/`flipY`
são booleanos.

```lua
draw.sprite(caminho, sx, sy, sl, sa, x, y [, largura, altura])
```

Recorta um pedaço da imagem — é assim que se anima uma spritesheet:

```lua
local frame = math.floor(engine.time() * 10) % 4
draw.sprite("assets/heroi.png", frame * 32, 0, 32, 32, heroi.x, heroi.y)
```

### Texto

```lua
draw.text(texto, fonte, tamanho, x, y [, r, g, b, a])
draw.measure(texto, fonte, tamanho)   --> largura, altura
```

`fonte` é o caminho de um `.ttf`. Exemplo de fonte quase sempre presente no
Linux: `/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf`.

```lua
local txt = "FPS: " .. math.floor(engine.fps())
local w = draw.measure(txt, FONTE, 16)
draw.text(txt, FONTE, 16, 640 - w - 10, 10, 255, 220, 120)
```

### Outros

```lua
draw.body(corpo [, r, g, b, a])   -- desenha a caixa de colisão
draw.clearCache()                 -- libera texturas e fontes em memória
```

---

## 8. API — input

### Teclado

| Função | Verdadeiro quando |
|---|---|
| `input.key(nome)` | a tecla está **segurada** |
| `input.pressed(nome)` | a tecla **desceu neste frame** |
| `input.released(nome)` | a tecla **subiu neste frame** |

A diferença importa: use `key` para andar (repete todo frame) e `pressed` para
pular, atacar ou abrir menu (dispara uma vez só).

Os nomes seguem o padrão do SDL: `"A"`, `"Z"`, `"Space"`, `"Left"`, `"Right"`,
`"Up"`, `"Down"`, `"Return"`, `"Escape"`, `"Left Shift"`, `"1"`… Nome inválido
gera erro, o que ajuda a pegar typo cedo.

### Mouse

```lua
local mx, my = input.mouse()

input.mouseDown(botao)      -- segurado
input.mousePressed(botao)   -- clicou neste frame
input.mouseReleased(botao)  -- soltou neste frame
```

`botao`: `1` esquerdo, `2` meio, `3` direito (padrão `1`).

---

## 9. API — body

Corpos são retângulos com nome. Um corpo **estático** não se move e serve de
cenário; um corpo **dinâmico** sofre gravidade e colide com os estáticos.

```lua
local chao  = body.static("chao", 0, 320, 640, 40)
local heroi = body.dynamic("heroi", 60, 100, 28, 40)
```

Ambos retornam um objeto. Criar um corpo com um nome já existente **substitui**
o antigo em vez de duplicar.

### Campos

| Campo | Tipo | Leitura | Escrita |
|---|---|---|---|
| `x`, `y` | número | ✓ | ✓ |
| `width`, `height` | número | ✓ | ✓ |
| `vx`, `vy` | número | ✓ | ✓ |
| `dynamic` | booleano | ✓ | ✓ |
| `solid` | booleano | ✓ | ✓ |
| `grounded` | booleano | ✓ | — |
| `name` | texto | ✓ | — |

`vx` e `vy` são em **pixels por segundo** — o motor multiplica por `dt`, então
não multiplique de novo.

`solid = false` transforma o corpo em sensor: continua detectando sobreposição
com `collides`, mas não empurra nem é empurrado. Serve para moeda, checkpoint,
área de dano.

`grounded` só é verdadeiro no frame em que o corpo encostou em algo por baixo.
É a condição certa para permitir pulo.

### Métodos

```lua
corpo:draw([r, g, b, a])
corpo:collides(outro)          --> booleano
corpo:move(dx, dy)
corpo:setPosition(x, y)
corpo:setVelocity(vx, vy)
corpo:center()                 --> cx, cy
corpo:destroy()
```

### Funções do módulo

```lua
body.get("nome")               --> corpo ou nil
body.collides(a, b)            --> booleano
body.destroy(corpo)
```

`collides` aceita tanto o objeto quanto o nome em texto:

```lua
if body.collides("heroi", "moeda") then ... end
if heroi:collides(moeda) then ... end
```

Usar um corpo já destruído gera erro em vez de ler memória inválida.

---

## 10. API — world

```lua
world.gravity(2000)     -- define (pixels por segundo ao quadrado)
local g = world.gravity()
```

Padrão: `1500`. Gravidade zero (`world.gravity(0)`) transforma o motor num
mundo de cima pra baixo, tipo top-down — os corpos dinâmicos passam a se mover
só pela velocidade que você definir.

---

## 11. Como a física funciona

A cada frame, para cada corpo dinâmico:

1. `vy` recebe a gravidade multiplicada por `dt`
2. o corpo move no **eixo X** e resolve colisões só nesse eixo
3. o corpo move no **eixo Y** e resolve colisões só nesse eixo
4. `grounded` vira verdadeiro se houve colisão por baixo

Resolver um eixo de cada vez é o que impede o personagem de travar em quina de
parede ou "subir degrau" sem querer — problema clássico de quem resolve
colisão pelo menor deslocamento nos dois eixos ao mesmo tempo.

Ao bater, a velocidade naquele eixo vira zero e o corpo é encostado exatamente
na borda do obstáculo.

**Dinâmico contra dinâmico não colide.** Só dinâmico contra estático. Para
detectar dois personagens se tocando, use `collides` e trate na mão — isso é
proposital: evita empurrões em cadeia e mantém a simulação previsível.

---

## 12. Desempenho

O motor mantém três caches internos:

| Cache | Chave | Efeito |
|---|---|---|
| Texturas | caminho do arquivo | a imagem é lida do disco uma única vez |
| Fontes | caminho + tamanho | o `.ttf` é aberto uma única vez |
| Texto | texto + fonte + tamanho + cor | a rasterização acontece uma vez |

Ou seja: chamar `draw.image` e `draw.text` todo frame é barato. O cache de
texto é limpo sozinho quando uma entrada passa 600 frames sem uso, então texto
dinâmico (contador de pontos, relógio) não vaza memória.

Outras decisões:

- vsync ligado por padrão, em vez de queimar CPU em loop
- círculo desenhado com linhas horizontais, O(raio) em vez de O(raio²)
- nomes de tecla resolvidos uma vez e guardados em cache
- `SDL_BLENDMODE_BLEND` ativo, então o canal alpha realmente funciona

Se estiver em máquina fraca, o maior ganho costuma ser reduzir a resolução da
janela e desenhar sprites já no tamanho final, evitando escalonamento.

---

## 13. Erros e depuração

Erro dentro de `update()` ou `render()` **não derruba o processo**. O motor
imprime a mensagem com o traceback completo e encerra o loop de forma limpa:

```
[LuaSun] erro em update():
game/main.lua:18: attempt to index a nil value (global 'heroi')
stack traceback:
        game/main.lua:18: in function 'update'
        [C]: in field 'run'
```

Erros de API (imagem faltando, fonte inexistente, corpo destruído) são erros
de Lua normais e podem ser capturados:

```lua
local ok, err = pcall(draw.image, "sprite.png", 0, 0)
if not ok then print("sem sprite:", err) end
```

Para desenhar as caixas de colisão por cima da arte enquanto depura:

```lua
if DEBUG then
    heroi:draw(255, 0, 0, 90)
    chao:draw(0, 255, 0, 90)
end
```

---

## 14. Limitações conhecidas

- Sem áudio (SDL_mixer não está incluído)
- Sem câmera: as coordenadas são sempre de tela
- Sem colisão dinâmico × dinâmico resolvida automaticamente
- Sem rotação em corpos físicos — a colisão é sempre AABB alinhada aos eixos
- Sem gamepad
- Uma janela por processo

Nenhuma dessas é difícil de acrescentar; o motor é um arquivo só, e cada
módulo da API tem menos de cem linhas.
