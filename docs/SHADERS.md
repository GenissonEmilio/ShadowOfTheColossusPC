# Shaders e FSR 1 no desktop

Na aba **Gráficos**, use **Importar shader…**. O port aceita um fragment shader
GLSL 330 em UTF-8, com extensão `.frag`, `.fs` ou `.glsl`, até 256 KiB. Ele é
compilado e vinculado no driver local antes de ser selecionado. Se houver erro,
a seleção anterior permanece ativa. O diagnóstico do driver fica em
`last-shader.log` na biblioteca.

O arquivo é copiado para `shaders/` na biblioteca. Sua origem pode ser movida
depois. O shader escolhido é salvo em `shader.json` e usado na próxima abertura.
Se o driver rejeitar um shader durante a abertura, o jogo usa a imagem sem
shader importado e registra a falha em `last-game.log` e `shader-status.txt`.

## Resolução e FSR 1

Use **F1 → Vídeo → Resolução da janela** para escolher um tamanho de janela.
Em tela cheia, o jogo usa a resolução do monitor. **F1 → Qualidade → Resolução**
controla a resolução interna da renderização, a partir de 512 × 448.

### Perfis gráficos

Escolha um perfil na aba **Gráficos** do launcher ou em **F1 → Qualidade**.
O perfil Original mantém o padrão da versão anterior para novas instalações.
As configurações existentes são carregadas ao abrir o jogo.

| Perfil | Resolução interna | FXAA | Filtro anisotrópico | Detalhe dos modelos | Reflexão e refração |
| --- | --- | --- | --- | --- | --- |
| Baixo | 256 × 224 | Desligado | Bilinear | Jogo | 256 px |
| Original | 512 × 448 | Desligado | Trilinear | Jogo | 256 px |
| Equilibrado | 768 × 672 | Ligado | 4× | Jogo | 256 px |
| Aprimorado | 1280 × 1120 | Ligado | 8× | Um nível acima | 512 px |
| Ultra | 2048 × 1792 | Ligado | 16× | Máximo | 1024 px |

O filtro anisotrópico usa até o limite do driver. Os perfis Aprimorado e Ultra
custam mais tempo e memória da GPU. Em F1 é possível ajustar separadamente a
resolução interna, o FXAA, o filtro, o detalhe dos modelos e o detalhe da água.
O FXAA atua na imagem do jogo, inclusive nos elementos 2D já compostos pelo
próprio jogo. A redução de uma imagem interna maior usa a média da área coberta
por cada pixel da saída, suavizando contornos com superamostragem.

### Ampliação

**F1 → Vídeo → Upscaling FSR 1** ativa o FSR 1 da AMD. Ele amplia a imagem
quando a área de exibição é maior que a imagem renderizada, nos modos Ajustar
e Esticar. A resolução interna pode ser reduzida para aliviar a renderização.
O modo Nítido mantém o fator de escala inteiro.

## Interface do fragment shader

```glsl
#version 330 core
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec2 sourceSize;
uniform vec2 outputSize;
uniform int fsrSharpened;
uniform float time;
out vec4 finalColor;
void main() {
    finalColor = texture(texture0, fragTexCoord) * colDiffuse * fragColor;
}
```

- `texture0`: imagem do jogo que chega ao shader após a redução ou o FSR 1
  e o FXAA, quando ativos.
- `sourceSize`: largura e altura da imagem que chega ao shader, em pixels.
- `outputSize`: largura e altura da imagem apresentada, em pixels físicos,
  sem incluir as faixas pretas. Considera a escala da tela.
- `time`: segundos desde a inicialização do runtime.
- `fsrSharpened`: 1 quando o passo RCAS do FSR 1 já aplicou nitidez, 0 nos
  demais casos. Um shader de nitidez pode usar esse valor para evitar o acúmulo.
- `fragTexCoord`: UV já orientada pelo apresentador; não inverta Y novamente.

Uniforms não utilizados podem ser omitidos. O estágio de vértices padrão é
fornecido por [raylib 5.5](https://github.com/raysan5/raylib/blob/5.5/src/rcore.c).
O pipeline aplica o shader importado em uma passagem depois do FXAA. O menu
F1, o contador de FPS e as barras pretas são desenhados na resolução da janela.

**Nitidez HD** é um filtro moderado de nitidez sobre a imagem apresentada.
O filtro limita a nitidez à faixa de cores vizinhas para reduzir halos e pula
esse passo quando o RCAS está ativo. Se você já tinha importado a versão
anterior, escolha **Usar Nitidez HD** novamente para importar a atual.
Presets multipass, dependências externas, texturas auxiliares, includes, ReShade
e presets RetroArch precisam ser adaptados para esta interface de passagem única.

## Teste sem ROM

`compiler/bin/sotc-video-tool --validate arquivo.frag --report resultado.json`
valida o shader em uma janela OpenGL oculta. O código de saída é zero apenas
quando a compilação e a vinculação pelo driver funcionam.

`--self-test pasta` abre uma imagem sintética e verifica os mesmos caminhos de
tela cheia, resolução de janela, shader e FSR 1 usados pelo jogo.
`tools/linux/check_video.py pasta`
confere dimensões e pixels do relatório. Esses testes fazem parte do CI.
