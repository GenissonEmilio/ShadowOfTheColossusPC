## Português

Esta atualização corrige a preparação e completa o suporte a save states no Windows.

- Corrige falhas do compilador/linker quando o perfil, a pasta do programa ou os temporários contêm acentos e outros caracteres Unicode. Inclui o erro de preparação relatado na issue #38.
- Corrige caminhos de módulos entre unidades, nomes com `@`, separadores Windows e a reutilização de objetos após uma compilação interrompida.
- Habilita **F5/F8 e F1 → Estados** no modo nativo americano do Windows, com estado aleatório compatível com o CRT, gravação atômica Unicode e restauração do histórico gráfico. O cartão PS2 continua sendo preservado.
- A carga de CPU no log Windows passa a usar o tempo efetivamente consumido por cada thread. Os diagnósticos de crash incluem versão do ntdll e limites de pilha. Mantém as correções de inicialização entregues na v1.2.5.
- Não muda resolução, qualidade gráfica ou limites de FPS. A comparação diagnóstica em janela oculta não mostrou perda no trecho avaliado; não representa uma promessa de ganho em outros equipamentos.

**Validação:** CI Windows, Linux e Android; preparação completa da ISO americana; link real com 568 objetos em caminho Unicode; F5/F8 antes e depois de reiniciar; preservação do slot bloqueado; rejeição de estado corrompido; histórico gráfico nas duas rotas de bloom. Todos os testes locais do jogo ocorreram em janela oculta e sem áudio. Os detalhes e limites estão na [PR #40](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/pull/40).

A primeira preparação pode recompilar automaticamente o jogo porque o runtime mudou. Não apague seus cartões de memória. Estados rápidos exigem o mesmo executável que os criou; use o save normal do jogo para levar o progresso entre versões. O PAL mantém seu modo atual e as respectivas restrições de save states.

**Android:** acompanha o instalador oficial ARM64 **0.2.1**, inalterado, com a assinatura original. Não é um APK Android 1.2.7 novo. O instalador público continua usando um pacote preparado localmente a partir do seu próprio disco. Nenhuma ISO ou dado extraído do jogo é distribuído.

Se você relatou um problema, teste esta versão e confirme o resultado na sua issue. Se a preparação falhar, anexe o novo `last-prepare.log`; se o jogo fechar ou continuar com quedas de desempenho, anexe `last-game.log`, a edição do disco e as configurações usadas.

## English

This update fixes Windows preparation and completes Windows save-state support.

- Fixes compiler/linker failures with accented and other Unicode paths, including the preparation failure reported in #38.
- Fixes cross-drive module paths, `@` in names, Windows path separators, and interrupted object-cache rebuilds.
- Enables **F5/F8 and F1 → States** in the default US native Windows mode, with CRT-compatible random state, atomic Unicode file writes and GPU history restoration. PS2 memory cards remain preserved.
- Windows CPU diagnostics now measure actual thread CPU time. Crash logs include the ntdll version and stack limits. The startup fixes first released in v1.2.5 remain included.
- Resolution, graphics quality and FPS limits are unchanged. A short hidden-window comparison showed no loss in the measured scene; it does not establish a general FPS gain.

**Validation:** Windows/Linux/Android CI, full US-disc preparation, a real link with 568 objects under Unicode paths, save/load across process restarts, preservation of a locked slot, rejection of a damaged state, and both GPU bloom-history paths. Local game tests used hidden, muted windows. See [PR #40](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/pull/40) for evidence and limits.

The first preparation may automatically rebuild the game because the runtime changed. Keep your memory cards. Quick states require the executable that created them; use the game's regular save to carry progress across builds. PAL retains its current mode and save-state restrictions.

**Android:** the unchanged, officially signed ARM64 **0.2.1 installer** is included. This is not a new Android 1.2.7 APK. It still needs a game bundle prepared locally from your own disc. No ISO or extracted game data is distributed.

If you reported a problem, please test this version and confirm the result in your issue. Attach the new `last-prepare.log` for preparation failures, or `last-game.log` with the disc edition and settings for startup crashes or persistent performance drops.
