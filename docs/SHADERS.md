# Shaders no Linux

Na aba **Gráficos**, use **Importar shader…**. O port aceita um fragment shader
GLSL 330 em UTF-8, com extensão `.frag`, `.fs` ou `.glsl`, até 256 KiB. Ele é
compilado e vinculado no driver local antes de ser selecionado. Se houver erro,
a seleção anterior permanece ativa; o diagnóstico do driver fica em
`~/.local/share/ShadowOfTheColossusPC/last-shader.log`.

O arquivo é copiado para `shaders/` na biblioteca. Sua origem pode ser movida
depois. O shader escolhido é salvo em `shader.json` e usado na próxima abertura.
Se o driver rejeitar um shader durante a abertura, o jogo usa a imagem sem
filtro e registra a falha em `last-game.log` e `shader-status.txt`.

## Interface do fragment shader

```glsl
#version 330 core
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec2 sourceSize;
uniform vec2 outputSize;
uniform float time;
out vec4 finalColor;
void main() {
    finalColor = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
}
```

- `texture0`: imagem original do jogo.
- `sourceSize`: largura e altura da imagem de origem, em pixels.
- `outputSize`: largura e altura da imagem apresentada, em pixels físicos,
  sem incluir as faixas pretas. Considera a escala da tela.
- `time`: segundos desde a inicialização do runtime.
- `fragTexCoord`: UV já orientada pelo apresentador; não inverta Y novamente.

Uniforms não utilizados podem ser omitidos. O estágio de vértices padrão é
fornecido por [raylib 5.5](https://github.com/raysan5/raylib/blob/5.5/src/rcore.c).
O pipeline aplica uma passagem ao desenhar a imagem no destino. O menu F1, o
contador de FPS e as barras pretas permanecem sem o filtro.

**Nitidez HD** é um filtro moderado de nitidez sobre a imagem ampliada. Não é
um pacote de texturas HD nem renderização de geometria em resolução superior.
Presets multipass, dependências externas, texturas auxiliares, includes, ReShade
e presets RetroArch precisam ser adaptados para esta interface de passagem única.

## Teste sem ROM

`compiler/bin/sotc-video-tool --validate arquivo.frag --report resultado.json`
valida o shader em uma janela OpenGL oculta. O código de saída é zero apenas
quando a compilação e a vinculação pelo driver funcionam.

`--self-test pasta` abre uma imagem sintética e verifica os mesmos caminhos de
tela cheia e shader usados pelo jogo. `tools/linux/check_video.py pasta`
confere dimensões e pixels do relatório. Esses testes fazem parte do CI.
