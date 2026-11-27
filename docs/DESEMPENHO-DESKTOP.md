# Desempenho no desktop

A análise parte da v1.2.9 (`1746b71`). A branch candidata traz para essa base
os quatro commits do [PR #42][pr] e otimiza o avanço de ciclos no interpretador
da VU1. As duas edições percorrem caminhos diferentes de renderização.

## Evidência dos relatos

| Edição e máquina | Observação nos logs | O que vale medir primeiro |
| --- | --- | --- |
| USA, Windows, Ryzen 7 2700U e Radeon RX 560X | Os quatro logs identificam a RX 560X no OpenGL. No quarto log, entre as amostras de 25 e 50 s, a janela apresenta perto de 100 vezes/s, com 19,2 a 28,3 quadros novos/s e zero interpolados. A contagem de instruções VU1 é zero nesses intervalos. | CPU da thread do jogo e da thread de renderização, espera entre elas, custo de apresentação, versão do driver e resolução interna. |
| PAL, Linux, Core i3-2330M e Intel HD 3000 | No log da v1.2.6, a janela fica perto de 60 apresentações/s enquanto a cena cai para 1–2 quadros novos/s. Na amostra de 60 s há 24,7 milhões de instruções VU1 em cerca de 5 s. O custo médio informado para o worker GS é 150,3 ms por quadro, incluindo 106,9 ms em desenho. | VU1/VIF1 e preparação/envio dos draws do GS, com perfil da mesma cena. |

Os números vêm da [issue #23][issue], do [quarto log da RX 560X][rx-log] e do
[log PAL da v1.2.6][pal-log]. As sessões usaram cenas e configurações variadas.
Os contadores de carga dos logs antigos do Windows inferiam ocupação por tempo
de espera. Na v1.2.9, a linha `[load] source=thread-cpu` registra tempo de CPU
por thread, incluindo o renderizador nativo e a janela.

## Caminho do frame e alterações

| Trecho | Comportamento na v1.2.9 e na candidata |
| --- | --- |
| Launcher e recompilador | O launcher seleciona renderização nativa para USA e compatibilidade OpenGL para PAL. O executável recompilado usa GCC, `-O2` e SSE4.1 nos dois desktops. O PCH altera o tempo de preparação do jogo. |
| EE, IOP, VIF e VU1 | No USA, os overrides evitam rasterização pelo GS quando o nativo assume a cena e o VIF1 pode percorrer os pacotes sem executar a geometria da VU1. No PAL, parte importante da cena passa pelo interpretador. Agora o interpretador avança trechos de espera até a próxima escrita pendente ou até o limite de ciclos. O commit da LSU continua precedendo o avanço de PATH1 no ciclo correspondente. |
| Renderização nativa OpenGL | A transferência normal usa texturas compartilhadas com fences, evitando readback síncrono, cópias na RAM e reenvio da imagem pela thread da janela. O readback continua disponível para diagnóstico e como fallback. Uma textura RGBA de 512 × 448 tem 0,875 MiB. A 1024 × 896, tem 3,5 MiB. |
| Composição da cena | A candidata busca apenas as camadas do passe atual, lê diretamente a imagem em efeitos que usam uma cópia reduzida e agrupa desenhos alpha compatíveis quando a cobertura é disjunta. Ordem, testes de profundidade e efeitos continuam cobertos pelos testes de imagem. |
| Janela, VSync e geração de quadros | O limitador da janela, o VSync e a interpolação afetam a apresentação. `realFps` conta imagens novas apresentadas, `generatedFps` conta interpoladas e `repeatedFps` conta repetições. `gameFps` é uma aproximação pela contagem de cadeias VIF1. |
| Áudio e leitura do disco | O jogo USA usa o caminho nativo de IOP do launcher. Leitura e áudio podem contribuir para picos de carregamento. Os logs disponíveis não isolam custo de armazenamento ou de áudio nas quedas sustentadas. |

O [PR #42][pr] observou, em um trecho Linux com Core i5-13420H e Intel UHD,
aproximadamente 59,6 quadros novos/s nas duas versões a 1×. A 2×, com dois
núcleos de eficiência disponíveis, os dois testes da v1.2.7 marcaram 30,8 e
37,4 quadros novos/s e os dois testes da candidata marcaram 59,3. O p95
mediano por janela passou de 37,1–39,5 ms para 17,3–17,4 ms. Esse resultado
foi publicado pelo PR original e compara com a v1.2.7 nessa rota.

## Verificação da candidata

- Runtime Release compilado no Linux e compilado e linkado para Windows x64
  com MinGW. O ambiente de compilação cruzada não executou WGL nem o jogo.
- Testes OpenGL com Mesa por software: 144 quadros alpha/depth idênticos
  byte a byte ao caminho ordenado, com 360 chamadas de desenho a menos no
  conjunto. Passaram os testes de ordem de cópia, DoF, texturas compartilhadas,
  redimensionamento e estado temporal dos dois caminhos de bloom.
- 42 testes de VU1 e timing PAL em modo exato passaram antes e depois da
  otimização. O teste acrescentado verifica commits de Q e P separados e
  retomada no limite de ciclos. O teste MSCAL passou a ler a VRAM pela API
  que aguarda a fila do GS.
- Microbenchmark Linux, AMD EPYC 9V74, GCC 14 Release, 30 milhões de ciclos
  convidados em um loop de EFU/WAITP. Sete pares alternados deram medianas de
  90,3 ms e 70,9 ms, cerca de 22% menos tempo nesse loop, com ciclos, PC e P
  iguais nas duas versões. O loop foi feito para concentrar esperas.
- Passaram os 78 testes Python do launcher Linux, dois testes Python de
  manifesto Windows e o teste desktop de contagem e cadência de quadros.

## Comparação em Windows de entrada

Use primeiro o Ryzen 7 2700U/RX 560X do relato e um PC com GPU integrada
compatível com OpenGL 3.3. Compare a v1.2.9 com a candidata, com a mesma
edição de disco, save, driver, resolução da janela e configuração de energia.
Aqueça cada build uma vez e meça três repetições por trecho no santuário,
no mundo aberto e em uma luta com efeitos. Registre também uma rota PAL
para o caso do interpretador.

1. Comece no perfil Baixo (256 × 224), sem shader externo, FSR ou geração de
   quadros. Use o mesmo VSync nas duas versões. Repita no Original
   (512 × 448) e nas configurações em que o problema apareceu.
2. Arquive o `last-game.log` de cada execução e anote os segundos de início
   e fim da cena. O launcher grava o arquivo em
   `%LOCALAPPDATA%\ShadowOfTheColossusPC` por padrão. As linhas
   `[stats]`, `[display]` e `[load]` mostram quadros novos, p95/p99 de
   intervalo, caminho de transferência e carga por thread.
3. Para isolar a contribuição da apresentação, compare as duas execuções
   da **mesma candidata**, com o comportamento padrão e com
   `SOTC_NATIVE_SHARED_TEXTURE=0` definido antes de abrir o launcher.
   `nativeShared` indica quadros entregues como textura compartilhada e
   `nativeSamples` indica o caminho de cópia pela CPU. Faça perfis de
   CPU separados com `SOTC_FRAME_LOG=1` (USA) ou `PS2X_VU1_PROFILE=1` (PAL).
4. Use 30 quadros novos/s sustentados e p95 de intervalo até 40 ms nas
   cenas jogáveis como alvo inicial de comparação. Registre também p99 e
   o maior intervalo, além de verificar velocidade do jogo, áudio, água,
   neblina, desfoque, transições de área e F5/F8 no USA. Compare cada cena
   separadamente, com a mesma cadência de tela.

Se o USA continuar limitado no perfil Baixo e a thread EE estiver saturada,
o próximo perfil deve localizar quais rotinas de jogo, skinning e preparação
de comandos consomem a CPU. Se a queda continuar concentrada no PAL, priorize
perfil de VU1, conversão VIF e lotes GS antes de promover o renderer PAL
experimental ou os lotes VU1 na GPU. A [documentação PAL][pal] e o
[plano de VU1][vu1] descrevem os caminhos atuais.

[issue]: https://github.com/GenissonEmilio/ShadowOfTheColossusPC/issues/23
[rx-log]: https://github.com/user-attachments/files/35373706/last-game.log
[pal-log]: https://github.com/user-attachments/files/35451752/last-game.SCES-53326.E.PAL.log
[pr]: https://github.com/GenissonEmilio/ShadowOfTheColossusPC/pull/42
[pal]: PAL-SUPPORT.md
[vu1]: vu1_fast_recompiler.md
