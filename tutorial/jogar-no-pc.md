# Como jogar no PC (teclado e mouse)

Guia para jogar o port Linux de Shadow of the Colossus com teclado e mouse,
no estilo dos jogos de PC: **WASD** move o Wander e o **mouse** controla a
câmera. Controles USB/Bluetooth também funcionam ao mesmo tempo.

## Abrir o jogo

- **Pelo launcher (release):** abra `ShadowOfTheColossusPC` e clique em
  **Iniciar jogo**.
- **Pelo repositório:** na pasta do projeto, rode `./celosia run`.

## Capturar o mouse

1. Clique uma vez dentro da janela. O cursor some e o mouse passa a mover a
   câmera. Esse primeiro clique não conta como golpe.
2. **Esc** pausa o jogo e solta o mouse. Clique na janela de novo para voltar.
3. Abrir o menu **F1** ou trocar de janela (Alt+Tab) também solta o mouse.

## Controles

| Teclado / mouse | Ação no jogo | Botão do PS2 |
| --- | --- | --- |
| **W A S D** | andar e correr | analógico esquerdo |
| **Alt esquerdo** (segurar) | andar devagar | analógico esquerdo pela metade |
| **mover o mouse** | câmera / mirar | analógico direito |
| **Espaço** | pular | △ |
| **botão esquerdo** | usar a arma (golpe de espada, tiro de arco) | □ |
| **botão direito** ou **F** (segurar) | agarrar (pelos do colosso, bordas) | R1 |
| **E** ou **Enter** | chamar o Agro, acelerar o Agro, confirmar | ✕ |
| **Shift esquerdo** | erguer a espada (o raio de luz mostra o colosso) | ○ |
| **Tab** ou **botão do meio** | olhar para o colosso | L1 |
| **rodinha** ou **1 / 2** | trocar de arma (espada, arco, mão livre) | direcional ← → |
| **setas** | direcional | direcional |
| **Z / X** | L2 / R2 | L2 / R2 |
| **C / V** | L3 / R3 | L3 / R3 |
| **Esc** | pausa (e solta o mouse) | Start |
| **Backspace** | Select | Select |
| **F1** | menu de configurações do port | — |
| **F11** | tela cheia | — |

Nos menus do jogo, use as **setas** (ou **WASD**) para navegar e **E/Enter**
para confirmar.

## Dicas de jogo

- **Encontrar o colosso:** erga a espada com **Shift** num lugar com sol e
  gire a câmera com o mouse até o raio de luz se juntar num ponto. Siga essa
  direção.
- **Escalar:** segure **botão direito** (ou **F**) para se agarrar. Solte para
  largar. A barra de força (círculo rosa) diminui enquanto você segura.
- **Golpear o ponto fraco:** agarrado ao colosso, segure o **botão esquerdo**
  para carregar o golpe e solte para cravar a espada.
- **Agro:** **E** chama o cavalo. Montado, **E** acelera e **WASD** conduz.
- **Arco:** troque para o arco com a **rodinha** (ou **1/2**), mire com o
  mouse e segure/solte o **botão esquerdo** para atirar.

## Ajustes

Variáveis de ambiente (defina antes de abrir o jogo):

| Variável | Efeito | Padrão |
| --- | --- | --- |
| `PS2X_MOUSE_SENS` | sensibilidade do mouse | `1` |
| `PS2X_MOUSE_INVERT_Y=1` | inverte o eixo vertical da câmera | desligado |
| `PS2X_STICK_DEADZONE` | zona morta do analógico do controle (0 a 0.9) | `0.18` |
| `PS2X_RUMBLE` | força da vibração do controle (0 desliga) | `1` |
| `PS2X_NO_RUMBLE=1` | desliga a vibração | desligado |

Exemplo: `PS2X_MOUSE_SENS=1.5 PS2X_MOUSE_INVERT_Y=1 ./celosia run`

O menu **F1** ajusta resolução, filtro de texturas, bloom, sombras, reflexos,
contraste, saturação, brilho e nitidez. Ele também muda o idioma do menu do
port. As escolhas ficam salvas em `celosia_settings.cfg`.

No menu F1: **Q / E** trocam de aba, **↑ / ↓** escolhem a opção, **← / →**
mudam o valor, **Backspace** volta e **F1** fecha. As mudanças valem na hora.

### GPU integrada (Intel UHD / Iris, AMD Vega de notebook)

A lógica do jogo roda a 60 fps, mas uma GPU integrada pode não aguentar
resolução 3x nas cenas pesadas (o colosso na tela, a espada erguida, a luz do
templo). Configuração sugerida em **F1 → Qualidade**:

| Opção | Valor |
| --- | --- |
| Resolução | **2** (ou 1, se ainda cair) |
| Filtro de texturas | **Trilinear** |
| Detalhe dos modelos | **Escolha do jogo** |
| Detalhe dos efeitos | **Alto** (tira o quadriculado do fundo desfocado e da névoa) |

O contador no canto mostra `game` (a lógica do jogo) e `host` (o que a janela
consegue mostrar). Se `game` está em 60 e `host` cai, o limite é a GPU.

## Notebook (só touchpad)

- Muitos sistemas Linux desligam o touchpad enquanto você digita. Se isso
  estiver ligado, a câmera para enquanto você segura **WASD**. Desligue a
  opção "desativar touchpad ao digitar" nas configurações do sistema
  (no GNOME: Configurações → Mouse e touchpad).
- **F** substitui o botão direito (agarrar), e **Tab** substitui o botão do
  meio (olhar para o colosso).

## Salvar o progresso

O jogo salva como no PS2: reze num altar (santuário) e confirme o save. Os
saves ficam numa imagem de memory card (formato PCSX2, `Mcd001.ps2`):

- **Launcher:** `~/.local/share/ShadowOfTheColossusPC/player/memcards/`
- **`./celosia run`:** `memcards/` na pasta do projeto (ou a pasta indicada em
  `CELOSIA_MEMCARDS`)

Faça cópia desse arquivo de vez em quando: ele é o seu progresso inteiro.
