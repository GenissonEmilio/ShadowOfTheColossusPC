# Validação Linux 0.1.0

Build nativo original fixado em Celosia `e0bc43d6d4eb8cc1a022c4b277f396be3ee4fbbe`.
A implementação Linux do runtime corresponde ao commit
`8e9f3a8d72dcf41b605c291b0e0627c47080ab81`; os commits seguintes acrescentam
metadados, documentação, roteiros de validação e a correção de fontes do launcher.

## Pacote e primeira preparação

- Linux x86_64, compilador GCC 13 incluído, runtime C++20 e OpenGL 3.3.
- Kit de compilação: `1b65ad890494855b3f681e27`.
- Arquivo: `ShadowOfTheColossusPC-0.1.0-linux-x86_64.tar.gz`, 158096280 bytes.
- SHA-256: `9c00f9b5ce952c4c91916d809b813d909726ecb45d58ee0214aa912f808557a2`.

A primeira preparação foi executada **pelo launcher empacotado**, partindo do
arquivo 7z fornecido pelo usuário, em Ubuntu 24.04 isolado, sem rede, com a
instalação somente para leitura. O ambiente não tinha Python, GCC, G++, CMake
nem .NET instalados. Python, o compilador e as bibliotecas necessários vieram do próprio pacote;
CMake e .NET não foram usados nessa etapa.

Após o ajuste exclusivamente visual de fontes, o launcher foi reempacotado
com o mesmo kit nativo e seu boot foi novamente testado no ambiente isolado.

O processo importou o disco USA SCUS-97472, recompilou o boot, capturou os módulos
KERNEL → MANAGER → GAMECORE e compilou o executável Linux com duas tarefas.
O relatório final identificou 9024 funções: 8738 recompiladas e 286 atendidas
pelos serviços nativos. Registrou zero falhas de decodificação, zero instruções
não suportadas e zero falhas críticas. Seis avisos tratam de entradas indiretas
promovidas para destinos adicionais pelo recompilador.

## Verificações concluídas

| Verificação | Evidência / alcance |
| --- | --- |
| Importação e limites de arquivos | 9 testes com dados sintéticos: caminhos, links, duplicatas, orçamento, imagem truncada, seleção anterior e ambiente |
| Compilador portátil | Compila e vincula um programa sintético com o runtime e executa `--version`, sem ROM |
| Primeiro boot pelo launcher | 600 quadros, GAMECORE iniciado, backend OpenGL ativo, três capturas e relatório automático aprovado |
| Cache e mudança de caminho | Segunda preparação reutiliza o executável; mtime do binário, registro de conclusão e log não mudam, mesmo acessando os dados pelo caminho do host |
| GPU da máquina | Mesa Intel Graphics (RPL-P), OpenGL 4.6 / Mesa 26.2.4 |
| Novo jogo e entrada | Duas execuções locais: introdução, Wander no santuário, corrida pelo cenário e ações enviadas pelo controle virtual |
| Áudio | Fluxo de saída do processo ativo e sem mute; captura interna do mixer com amostras não nulas |
| Persistência do cartão | Página de 512 bytes gravada no backend nativo e relida/validada por outro processo; imagem de 8650752 bytes |

Os testes do host usaram cartões separados em pastas de diagnóstico. Os testes
de persistência usam dados sintéticos, sem alterar saves do jogador.

## Limites da verificação

Não foi feita uma campanha completa nem a luta contra todos os 16 colossos.
O teste de cartão verifica o backend e a persistência entre processos; não é
uma validação completa de salvar e carregar pelo menu do jogo. Os valores de
FPS observados nas cenas iniciais não são uma garantia para toda a campanha.
A decompilação manual legível da base continua em andamento; este release usa
recompilação estática do código completo encontrado no disco.

## Repetir os diagnósticos locais

`tools/linux/capture_gameplay.py` captura um roteiro de novo jogo, movimento,
imagens e áudio interno. As imagens precisam de revisão visual; o roteiro não
declara a campanha concluída. `tools/linux/check_cache.py` verifica a reutilização
do executável. `tools/linux/test_memory_card.py` testa o cartão sem dados do jogo.

O CI compila o pacote sem ROM, testa o compilador e a persistência do cartão com
dados sintéticos. ROM, código gerado do jogo, cartões e capturas de áudio ficam
fora dos artefatos publicados.
