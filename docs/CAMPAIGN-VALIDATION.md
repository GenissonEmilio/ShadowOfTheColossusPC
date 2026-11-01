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
| 1 | Valus | Derrotado e salvo no candidato cb15, com efeitos revisados; recarga no mesmo pacote escolhido para v0.2.7 pendente | — |
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

O jogo foi iniciado por New Game. O cartão novo foi formatado pela interface e salvo no slot 1; a tela confirmou `Shrine of Worship — 0 Defeated` e `Save complete`. Uma cópia desse cartão foi preservada como checkpoint privado. Esse salvamento inicial, isoladamente, não representa uma validação de colosso; a derrota posterior é descrita abaixo.

O percurso até Valus foi feito com entradas normais: trepadeiras, duas lacunas, rolagem sob o pilar, paredes, contorno do pilar final e salto para a cornija. As tentativas anteriores incluíram esgotamento de stamina, alvo de salto inadequado e hoist incompleto; elas não foram classificadas como reprodução inequívoca de atravessamento de chão. A captura privada `g462` documenta o começo da investigação da luta. O altar externo e a rampa de Gaius continuam pendentes.

No candidato com paleta ativa (`6191f4f`, kit `cb15`), Valus foi derrotado com comandos normais. Foram percorridos a panturrilha, as cornijas, as plataformas das costas e a cabeça. O círculo de carga cheio e o glifo brilhando na cabeça foram observados; as estocadas aceitaram dano na parte correta até sua capacidade de `200`. Um golpe na região adjacente de capacidade `0,130719` foi distinguido dos golpes na marca principal, sem ser classificado como defeito de dano.

O jogo confirmou `Save complete` no cartão próprio de QA: segundo registro, `Shrine of Worship — 1 Defeated / 1:04:39`. O primeiro registro de zero derrotados foi preservado para repetição. As flags originais passaram para um derrotado e próximo colosso, e o cartão pessoal conservou seu hash e data de modificação. A gravação foi finalizada e o jogo saiu normalmente pelo menu, com código zero.

A gravação contínua dessa derrota foi revisada: o corpo escurece sem o padrão de arco-íris nas imagens examinadas, e os tentáculos pretos saem do corpo e alcançam Wander. O trecho foi preservado com hashes e correspondência com os pacotes da gravação original. Essa evidência é do candidato `cb15/6191f4f`; ela não é uma observação da morte no pacote posterior de partículas.

O pacote escolhido para a **v0.2.7 planejada é o do CI de `5c84a62`, kit `cb15`**. Suas onze entradas de ligação nativa foram conferidas como idênticas às do pacote `6191f4f` usado na derrota acima; o launcher inclui a correção do atalho canônico. A recarga do registro de um derrotado nesse pacote exato ainda está pendente. A publicação da release também está pendente.

O pacote de partículas `d357` foi usado numa investigação separada: carregou normalmente o segundo registro, com um colosso derrotado e retorno ao templo, e depois o registro inicial para repetir Valus. Esse replay ficou incompleto. As tentativas não demonstraram um novo defeito do produto e não substituem a evidência da derrota no nativo `cb15`. A recarga em `d357` preservou os dois registros próprios de QA e não modificou o cartão pessoal.

**O progresso integralmente validado permanece em 0/16 até confirmar a recarga no pacote `5c84a62/cb15`.** A derrota, o salvamento e os efeitos de Valus já foram observados no nativo correspondente. As próximas etapas são confirmar esse checkpoint, publicar a etapa de Valus e prosseguir para Quadratus.

O controle automatizado de QA passou a observar a base original dos controles e a posição do pé durante a aproximação. Isso corrige a orientação dos comandos de teste; não altera os controles nem a física do produto.

Uma auditoria de leitura confirmou 19 rotinas de colisão originais e 31 substituições nativas ativas. As nativas de matemática, matrizes e personagem ainda podem influenciar a física; conservar as rotinas de colisão não prova fidelidade completa. Não houve evidência que justificasse alterar o limite de quadros ou o timestep nesta etapa.

O checkpoint inicial foi recarregado normalmente no candidato `6191f4f`, com `0 Defeated` e a posição do altar preservados. A tentativa de combate anterior causou dano real à perna de Valus, mas foi encerrada sem derrota antes da integração da paleta; nenhum progresso de colosso foi contabilizado. O HUD de carga foi observado aumentando durante a preparação de uma estocada.

O candidato escolhido contém as correções descritas em [gráficos](FIDELITY-2026-10-31-GRAPHICS.md) e [estado do personagem](FIDELITY-2026-10-31-CHARA.md). A [execução completa do CI](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/actions/runs/41245595672) passou para `83d8eff`. A correção da [paleta de transferência](TRANSFER-PALETTE-FIDELITY.md) também foi integrada; o [CI de `6191f4f`](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/actions/runs/41259725998) passou, incluindo os 22 casos da paleta. O executável da primeira derrota foi ligado às bibliotecas desse pacote exato. A release principal permanece em v0.2.6 até concluir os gates da v0.2.7. A investigação de tail jumps está em branch separada e ainda não integra o executável da campanha.

Evidências privadas e comandos da sessão: `artifacts/campaign-2026-10-31/`. ROM, cartões, RAM e código derivado do jogo não são publicados no GitHub.

O candidato do launcher também corrige atalhos recriados que ficavam presos ao diretório de uma versão antiga. Os sete casos do atalho e os demais testes passaram no [CI de `5c84a62`](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/actions/runs/41262842772). Esse é o pacote selecionado para a v0.2.7, com o nativo `cb15` que corresponde à primeira derrota validada em vídeo; sua recarga e publicação ainda precisam ser confirmadas.

O [material das partículas](PARTICLE-MATERIAL-FIDELITY.md) foi corrigido em `5ec48fe`: CLAMP por eixo e teste de profundidade sem escrita de Z, conforme o pacote original. O [CI completo](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/actions/runs/41272468970) passou. O pacote exato desse CI foi auditado e usado como candidato local, com kit `d357e14c82a02adeeca1aae3`; apenas o objeto nativo correspondente mudou na biblioteca base. Gerador, configuração, headers, toolchain e demais entradas de ligação permaneceram idênticos, permitindo religar os 284 objetos guest existentes sem recompilação completa. Essa alteração fica reservada à **candidata v0.2.8**, fora do pacote escolhido para v0.2.7. O replay incompleto não valida seus efeitos em combate, e os cenários específicos continuam pendentes. Ainda não há novo pacote v0.2.8 concluído ou release publicada por esta decisão.
