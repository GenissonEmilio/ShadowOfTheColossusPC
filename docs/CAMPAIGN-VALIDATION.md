# Validação da campanha — em andamento

Base inicial: Linux v0.2.6 (`986643a`). Nenhuma campanha completa foi validada nesta etapa ainda. Uma bateria sintética ou uma flag de derrota, isoladamente, não representa uma batalha jogada.

## Separação dos dados

A campanha de QA usa `~/.local/share/ShadowOfTheColossusPC-QA/campaign-2026-10-31/`. O cartão dessa pasta começou ausente e é criado pelo jogo. O cartão pessoal não foi copiado. ROM e executável compilado são reutilizados por leitura; saves, configurações e logs pertencem à pasta de QA. O controle virtual envia comandos normais de um gamepad Linux; não há escrita em RAM, warp, morte forçada de inimigo ou importação de progresso pronto.

As rotinas de colisão originais permanecem selecionadas: `SOTC_NATIVE_COLLISION` e `SOTC_DIFFTEST` ficam ausentes. A tela cheia do produto é preservada; a sessão de teste pode usar janela para observação.

## Critério de conclusão por colosso

1. Percorrer o acesso e jogar a luta com comandos normais. Observar terreno, plataformas, pelo/agarrar, mudanças de câmera e possíveis quedas pela geometria.
2. Conferir HUD, carga da estocada, pontos fracos, dano, partículas, efeitos específicos e sequência da derrota. Guardar evidências de tela e do renderizador quando houver divergência.
3. Correlacionar vídeo/capturas com as flags SCUS observadas por leitura; guardar o checkpoint de QA e verificar persistência ao carregar.
4. Corrigir defeitos reproduzidos e testar regressões antes de atualizar o launcher local/publicar a release correspondente. Uma correção apenas sintética não autoriza marcar um colosso como validado.

## Progresso

| # | Colosso | Situação | Release |
| --- | --- | --- | --- |
| 1 | Valus | Em andamento; checkpoint próprio salvo no Templo, 0 derrotados | — |
| 2 | Quadratus | Pendente | — |
| 3 | Gaius | Pendente, incluindo rampa de acesso | — |
| 4 | Phaedra | Pendente | — |
| 5 | Avion | Pendente | — |
| 6 | Barba | Pendente | — |
| 7 | Hydrus | Pendente, incluindo raios | — |
| 8 | Kuromori | Pendente | — |
| 9 | Basaran | Pendente | — |
| 10 | Dirge | Pendente | — |
| 11 | Celosia | Pendente | — |
| 12 | Pelagia | Pendente | — |
| 13 | Phalanx | Pendente | — |
| 14 | Cenobia | Pendente | — |
| 15 | Argus | Pendente | — |
| 16 | Malus e encerramento | Pendente | — |

## Checkpoint inicial e candidato

O jogo foi iniciado por New Game. O cartão novo foi formatado pela interface e salvo no slot 1; a tela confirmou `Shrine of Worship — 0 Defeated` e `Save complete`. Uma cópia desse cartão foi preservada como checkpoint privado. Ainda falta verificar a luta e a derrota de Valus; o salvamento inicial não é uma validação de colosso.

O candidato contém as correções descritas em [gráficos](FIDELITY-2026-10-31-GRAPHICS.md) e [estado do personagem](FIDELITY-2026-10-31-CHARA.md). A [execução completa do CI](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/actions/runs/41245595672) passou para `83d8eff`. A release principal permanece em v0.2.6 enquanto a primeira luta é validada. A investigação de tail jumps está em branch separada e ainda não integra o executável da campanha.

Evidências privadas e comandos da sessão: `artifacts/campaign-2026-10-31/`. ROM, cartões, RAM e código derivado do jogo não são publicados no GitHub.
