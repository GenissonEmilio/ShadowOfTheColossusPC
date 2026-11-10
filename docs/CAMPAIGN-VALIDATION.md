# Validação da campanha — em andamento

Base inicial: Linux v0.2.6 (`986643a`). Nenhuma campanha completa foi validada nesta etapa ainda. Uma bateria sintética ou uma flag de derrota, isoladamente, não representa uma batalha jogada.

## Separação dos dados

A campanha de QA usa `~/.local/share/ShadowOfTheColossusPC-QA/campaign-2026-10-31/`. O cartão dessa pasta começou ausente e é criado pelo jogo. O cartão pessoal não foi copiado. ROM e executável compilado são reutilizados por leitura; saves, configurações e logs pertencem à pasta de QA. O controle virtual envia comandos normais de um gamepad Linux; não há escrita em RAM, warp, morte forçada de inimigo ou importação de progresso pronto.

As rotinas de colisão originais permanecem selecionadas: `SOTC_NATIVE_COLLISION` e `SOTC_DIFFTEST` ficam ausentes. A tela cheia do produto é preservada; a sessão de teste pode usar janela para observação.

A investigação de Hydrus iniciada em 5 de novembro usa outra pasta isolada,
`ShadowOfTheColossusPC-QA/hydrus-2026-11-05`, com uma cópia do cartão de oito
derrotados fornecido pelo usuário. Esses oito derrotados importados **não são
oito batalhas validadas**. Novos registros nos altares e estados rápidos dessa
sessão pertencem somente à cópia de QA; o cartão pessoal e o cartão da campanha
inicial são preservados. A revanche de Hydrus foi iniciada pela oração normal
nos restos do colosso, sem alterar flags ou memória do jogo.

## Critério de conclusão por colosso

1. Percorrer o acesso e jogar a luta com comandos normais. Observar terreno, plataformas, pelo/agarrar, mudanças de câmera e possíveis quedas pela geometria.
2. Conferir HUD, carga da estocada, pontos fracos, dano, partículas, efeitos específicos e sequência da derrota. Guardar evidências de tela e do renderizador quando houver divergência.
3. Correlacionar vídeo/capturas com as flags SCUS observadas por leitura; guardar o checkpoint de QA e verificar persistência ao carregar.
4. Corrigir defeitos reproduzidos e testar regressões antes de atualizar o launcher local/publicar a release correspondente. Uma correção apenas sintética não autoriza marcar um colosso como validado.

## Progresso

**2/16 etapas validadas:** Valus no nativo `cb15` e Basaran no `0281`.
Os outros quatorze colossos e o encerramento permanecem pendentes. Basaran
foi concluído em 10 de novembro de 2026 com a correção comum de
[transparência e profundidade](FIDELITY-2026-11-07-ALPHA-DEPTH.md), destinada
à v0.3.4 junto da navegação por controle no launcher. A publicação exige
também a aprovação do pacote portátil; veja as versões disponíveis em
[Releases](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases).

A captura de água foi [verificada em 240 quadros de Hydrus](FIDELITY-2026-11-05-COPY-LAYERS.md).
A revanche de Hydrus foi concluída no runtime anterior, mas sua aprovação
visual integral continua pendente. A correção compartilhada de transparência
não transforma os demais colossos em batalhas validadas automaticamente.

| # | Colosso | Situação | Release |
| --- | --- | --- | --- |
| 1 | Valus | Validado no nativo cb15: acesso, luta, efeitos da derrota, salvamento e recarga | [v0.2.7](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases/tag/linux-v0.2.7) |
| 2 | Quadratus | Acesso investigado na v0.2.8; derrota ainda pendente | — |
| 3 | Gaius | Subida, piso e descida da rampa testados; luta e novo replay visual da água pendentes | — |
| 4 | Phaedra | Pendente | — |
| 5 | Avion | Pendente | — |
| 6 | Barba | Pendente | — |
| 7 | Hydrus | Revanche concluída: três espinhos, cabeça, derrota, retorno e F5/F8; brilho inequívoco do sigilo e aprovação visual integral ainda pendentes | [v0.3.3, correção de materiais](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/releases/tag/linux-v0.3.3) |
| 8 | Kuromori | Pendente | — |
| 9 | Basaran | Validado por etapas no mesmo nativo 0281: tornozelos, tombamento, escalada, sigilo com espada, carga, sete estocadas, derrota/sombras, salvamento normal e recarga em processo novo | v0.3.4 |
| 10 | Dirge | Pendente | — |
| 11 | Celosia | Pendente | — |
| 12 | Pelagia | Pendente | — |
| 13 | Phalanx | Pendente | — |
| 14 | Cenobia | Pendente | — |
| 15 | Argus | Pendente | — |
| 16 | Malus e encerramento | Pendente | — |

## Basaran — 10 de novembro de 2026

O checkpoint privado foi retomado no executável de SHA-256
`b235334eb8ca239219882ae4fdd00edb8d6e1018edcb087a30ef95767e428876`.
As duas flechas efetivas e o tombamento já estavam registrados nesse mesmo
executável; a continuação percorreu o pelo, a borda, o casco e a cabeça com
entradas normais. Não houve escrita em RAM, teleporte, dano forçado ou
alteração de tempo da simulação.

O sigilo foi capturado inteiro e luminoso antes de qualquer dano na cabeça,
com a espada equipada e a intensidade original em 1. O círculo de força foi
observado durante a preparação da estocada. Sete golpes reais levaram os
239,256 pontos de vida restantes a zero. O corpo escureceu, as sombras ficaram
visíveis e a absorção prosseguiu até o retorno ao templo. Uma captura teve
oclusão breve da câmera pelo corpo durante a queda; não se atribuiu esse
instante a uma divergência comprovada da versão original.

O jogo criou o slot 4 da cópia de QA: `Shrine of Worship — 9 Defeated`.
Os três slots importados anteriores foram preservados. Uma execução nova
carregou esse slot pela tela original, sem F8, e retornou ao templo com nove
derrotados, Basaran registrado e Dirge como próximo colosso. Os oito derrotados
importados não contam como oito lutas testadas. Cartões e configurações pessoais
não foram usados para gravar esse progresso.

Nos 62 intervalos ativos de um segundo amostrados na cabeça, o contador usado
pelo HUD variou de 55,47 a 60,56, com mediana 59,49. Isso mede listas de desenho
por segundo; não é uma medição de tempo de GPU nem uma garantia de desempenho
em toda a campanha. As pausas, restaurações e lacunas de observação foram
excluídas. Evidências privadas: `artifacts/hydrus-2026-11-05/`.

## Checkpoint inicial e candidato

As seções seguintes conservam o histórico da campanha inicial e das candidatas
daquela etapa. Suas referências à instalação então principal não substituem o
estado atual informado acima. A validação específica da rampa de Gaius está
documentada em [apoio do piso](FIDELITY-2026-11-03-GROUND-SUPPORT.md) e
[comparações signed](FIDELITY-2026-11-04-SIGNED-BRANCHES.md).

O jogo foi iniciado por New Game. O cartão novo foi formatado pela interface e salvo no slot 1; a tela confirmou `Shrine of Worship — 0 Defeated` e `Save complete`. Uma cópia desse cartão foi preservada como checkpoint privado. Esse salvamento inicial, isoladamente, não representa uma validação de colosso; a derrota posterior é descrita abaixo.

O percurso até Valus foi feito com entradas normais: trepadeiras, duas lacunas, rolagem sob o pilar, paredes, contorno do pilar final e salto para a cornija. As tentativas anteriores incluíram esgotamento de stamina, alvo de salto inadequado e hoist incompleto; elas não foram classificadas como reprodução inequívoca de atravessamento de chão. A captura privada `g462` documenta o começo da investigação da luta. O altar externo e a rampa de Gaius continuam pendentes.

No candidato com paleta ativa (`6191f4f`, kit `cb15`), Valus foi derrotado com comandos normais. Foram percorridos a panturrilha, as cornijas, as plataformas das costas e a cabeça. O círculo de carga cheio e o glifo brilhando na cabeça foram observados; as estocadas aceitaram dano na parte correta até sua capacidade de `200`. Um golpe na região adjacente de capacidade `0,130719` foi distinguido dos golpes na marca principal, sem ser classificado como defeito de dano.

O jogo confirmou `Save complete` no cartão próprio de QA: segundo registro, `Shrine of Worship — 1 Defeated / 1:04:39`. O primeiro registro de zero derrotados foi preservado para repetição. As flags originais passaram para um derrotado e próximo colosso, e o cartão pessoal conservou seu hash e data de modificação. A gravação foi finalizada e o jogo saiu normalmente pelo menu, com código zero.

A gravação contínua dessa derrota foi revisada: o corpo escurece sem o padrão de arco-íris nas imagens examinadas, e os tentáculos pretos saem do corpo e alcançam Wander. O trecho foi preservado com hashes e correspondência com os pacotes da gravação original. Essa evidência é do candidato `cb15/6191f4f`; ela não é uma observação da morte no pacote posterior de partículas.

O pacote publicado na **v0.2.7 é o do CI de `5c84a62`, kit `cb15`**. Suas onze entradas de ligação nativa foram conferidas como idênticas às do pacote `6191f4f` usado na derrota acima; o launcher inclui a correção do atalho canônico. O nativo conserva o SHA-256 `fa13630ce218387a16e59127564c1e857c1a7a1d821aba71c98ade97d8399d8e`.

A recarga foi confirmada nesse pacote: o menu original selecionou o segundo registro `1 Defeated / 1:04:39`, confirmou a leitura e retornou ao templo. A observação por leitura mostrou um colosso derrotado, Valus derrotado e Quadratus como próximo colosso; o hash do cartão próprio de QA permaneceu igual. A instalação principal e o atalho foram atualizados para a v0.2.7, preservando a v0.2.6 para rollback e os dados pessoais.

O pacote de partículas `d357` foi usado numa investigação separada: carregou normalmente o segundo registro, com um colosso derrotado e retorno ao templo, e depois o registro inicial para repetir Valus. Esse replay ficou incompleto. As tentativas não demonstraram um novo defeito do produto e não substituem a evidência da derrota no nativo `cb15`. A recarga em `d357` preservou os dois registros próprios de QA e não modificou o cartão pessoal.

**O progresso validado é 1/16 no pacote `5c84a62/cb15`.** A derrota, os efeitos, o salvamento e a recarga de Valus estão confirmados no nativo correspondente. A próxima etapa é Quadratus. O replay incompleto em `d357` permanece uma investigação separada.

O controle automatizado de QA passou a observar a base original dos controles e a posição do pé durante a aproximação. Isso corrige a orientação dos comandos de teste; não altera os controles nem a física do produto.

Uma auditoria de leitura confirmou 19 rotinas de colisão originais e 31 substituições nativas ativas. As nativas de matemática, matrizes e personagem ainda podem influenciar a física; conservar as rotinas de colisão não prova fidelidade completa. Não houve evidência que justificasse alterar o limite de quadros ou o timestep nesta etapa.

O checkpoint inicial foi recarregado normalmente no candidato `6191f4f`, com `0 Defeated` e a posição do altar preservados. A tentativa de combate anterior causou dano real à perna de Valus, mas foi encerrada sem derrota antes da integração da paleta; nenhum progresso de colosso foi contabilizado. O HUD de carga foi observado aumentando durante a preparação de uma estocada.

O pacote publicado contém as correções descritas em [gráficos](FIDELITY-2026-10-31-GRAPHICS.md) e [estado do personagem](FIDELITY-2026-10-31-CHARA.md). A [execução completa do CI](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/actions/runs/41245595672) passou para `83d8eff`. A correção da [paleta de transferência](TRANSFER-PALETTE-FIDELITY.md) também foi integrada; o [CI de `6191f4f`](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/actions/runs/41259725998) passou, incluindo os 22 casos da paleta. O executável da primeira derrota foi ligado às bibliotecas desse pacote exato. A release principal é a v0.2.7. A investigação de tail jumps está em branch separada e ainda não integra o executável da campanha.

Evidências privadas e comandos da sessão: `artifacts/campaign-2026-10-31/`. ROM, cartões, RAM e código derivado do jogo não são publicados no GitHub.

O launcher também corrige atalhos recriados que ficavam presos ao diretório de uma versão antiga. Os sete casos do atalho e os demais testes passaram no [CI de `5c84a62`](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/actions/runs/41262842772). Esse pacote está publicado e instalado como v0.2.7, com o nativo `cb15` da primeira etapa validada e recarregada.

O [material das partículas](PARTICLE-MATERIAL-FIDELITY.md) foi corrigido em `5ec48fe`: CLAMP por eixo e teste de profundidade sem escrita de Z, conforme o pacote original. O [CI completo](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/actions/runs/41272468970) passou. O pacote exato desse CI foi auditado e usado como candidato local, com kit `d357e14c82a02adeeca1aae3`; apenas o objeto nativo correspondente mudou na biblioteca base. Gerador, configuração, headers, toolchain e demais entradas de ligação permaneceram idênticos, permitindo religar os 284 objetos guest existentes sem recompilação completa. Essa alteração fica reservada à **candidata v0.2.8**, fora do pacote publicado na v0.2.7. O replay incompleto não valida seus efeitos em combate, e os cenários específicos continuam pendentes. O [CI da candidata v0.2.8](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/actions/runs/41284313971) passou e gerou um pacote, auditado e instalado separadamente como v0.2.8. Suas onze entradas de ligação, headers, toolchain e gerador/configuração coincidem com o candidato de partículas anterior; apenas documentação e rótulos de versão do launcher diferem. O cache nativo `d357` foi reutilizado sem nova compilação ou ligação. O registro de um derrotado foi carregado normalmente nessa instalação, e o percurso até Quadratus está em andamento. A derrota e os efeitos da segunda etapa ainda não foram validados. O launcher principal permanece na v0.2.7; a v0.2.8 não foi publicada.
