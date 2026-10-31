# Linux v0.2.5 — teclado e mouse

A câmera agora acompanha o movimento do mouse. O perfil inicial usa:

- WASD para movimentar e Espaço para pular.
- Clique esquerdo para atacar, direito para agarrar e central para ação/luz da espada.
- Q/E para trocar armas; F/R para L1/L2.
- Esc para Start, I para Select, C para Agro e Shift esquerdo para R2.
- Setas esquerda/direita e Page Up/Down também movem a câmera pelo teclado.

Em **Controles → Personalizar teclado e mouse**, cada ação pode receber outra
tecla ou botão. O editor oferece sensibilidade, inversão horizontal/vertical,
opção de desligar a câmera pelo mouse e restauração do perfil padrão. Mudanças
são salvas localmente e usadas na próxima abertura do jogo. F1 e F11 são reservados.

**Esc pausa sem encerrar.** A saída fica em **F1 → Sistema → Voltar ao launcher**,
com confirmação. As abas e itens do F1 também aceitam cliques. F1 libera o cursor;
a captura é suspensa ao trocar de janela. Comandos mantidos durante essa transição
precisam ser soltos antes de voltar ao jogo, para evitar ataques involuntários.
Controles USB/Bluetooth continuam funcionando ao mesmo tempo.

## Validação

- 25 testes Python: importação, cache, vídeo, cartões e persistência de controles.
- Teste nativo dos pacotes PS2: comandos simultâneos, remapeamento, mouse a
  60/120 Hz, bloqueio do menu, perda de foco e leitura pela thread do jogo.
- Editor GTK real: capturar tecla, atribuir Esc, salvar, recarregar e cancelar.
- Jogo carregado numa cópia do cartão Cliff Path: movimento da câmera confirmado
  pela matriz do jogo, botões do mouse e WASD/espaço observados na entrada nativa;
  Esc sem encerrar e saída pelo mouse no F1 com código 0.

Esta atualização trata dos controles. A travessia da rampa de Gaius e os relatos
anteriores de colisão e efeitos visuais continuam pendentes de confirmação; a
PR de investigação permanece aberta. ROM e saves não acompanham o pacote.
