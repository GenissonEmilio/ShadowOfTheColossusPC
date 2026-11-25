# Contextos VU0 e saves portáveis — 29/10/2026

As novas gravações mostram afundamento no altar (`174723`, 10,330 s), queda
através do meio da rampa (`175211`, 12,996 s) e a luta com Valus (`180525`,
189,163 s). Os arquivos `175211 (1)` e `(2)` têm o mesmo SHA-256 do `175211`
sem sufixo; são cópias do mesmo vídeo. A evidência do usuário continua válida.

## Defeito demonstrado e correção

O dispatcher copiava o arquivo completo de registradores VU0 do último contexto
executado para o próximo. Porém o código recompilado pode ceder no meio de uma
função, com VF/VI/ACC/Q e flags ainda vivos. Um callback ou outra thread que
usasse esses registradores substituía os intermediários do cálculo suspenso.
Isso afeta a integridade de cálculos vetoriais usados também pela colisão;
ainda não demonstra a causa exata das quedas filmadas.

A regressão sintética em `tests/native/fidelity.cpp` suspende um cálculo com
vetor de posição, normal, quociente, inteiro e flags conhecidos, executa um
callback que muda esses valores, e retoma o cálculo. **Falhou com a biblioteca
0.2.3**, no teste sem qualquer arquivo do jogo. Com a correção, passa também
com um segundo callback aninhado. O RNG avançado pelo callback continua visível
após o retorno, sem recomeçar a sequência em threads recém-criadas.

VF/VI/ACC/Q/flags permanecem no R5900Context suspenso. Somente o registrador
aleatório R é compartilhado. Seu valor é salvo tanto em retornos normais quanto
em transferências ao dispatcher. A leitura após a transferência relocaliza o
contexto pelo ID/profundidade/sequência: um callback aninhado pode realocar o
vetor de invocações, e ExitDeleteThread pode apagar o registro inteiro.

Foram preservadas as rotinas originais de colisão e a física do jogo. Nenhum
limite artificial de altura, teletransporte de resgate ou alteração de malha
foi incluído no produto. As tentativas privadas de preparar Gaius por transição
Time Attack permaneceram em tela preta; uma tentativa de reposicionar o ator
não carregou a região correta. **Esses experimentos não contam como validação
da rampa.**

## Validação local

- Regressões nativas: passaram preservação de VU0/RNG em callbacks aninhados,
  HUD, CLUT/RGBA, cache de geometria/textura, despacho de colisão original e
  reserva de pilhas sem sobreposição.
- Os 42 testes existentes de kernel/interrupções passaram; os 22 testes
  Python passaram.
- Caminhada lenta (analógico 0,67), com as configurações gráficas fornecidas:
  180,05 s, 721 amostras, 17 alvos atingidos, incluindo quatro subidas,
  quatro descidas e travessias laterais. Root Y entre 5,479736 e 5,798884;
  nenhuma queda observada no altar Cliff Path. Alvos interrompidos por tempo
  não são contados; este percurso atingiu todos os alvos antes do limite.
- Jogo de teste SHA-256
  `4e5ba9516a093cb8a391753a59648e7e0dabd934e06cb09609aae3d6360c26c8`,
  kit `2ae96501c03bdb39f1786118`. Os objetos traduzidos foram reutilizados
  somente com o ABI público e os 9.079 fontes gerados já conferidos como
  idênticos; o runtime e a biblioteca nativa são os novos.
- O pacote PyInstaller abriu as quatro páginas em Ubuntu limpo e iniciou
  GAMECORE/renderizador OpenGL no notebook Intel. Isso é teste de boot,
  não validação da campanha.

## Launcher

O antigo botão chamava Gtk.show_uri_on_window diretamente no processo
empacotado. O novo helper inicia xdg-open, com fallback para gio open, usando
um ambiente independente do bundle: restaura LD_LIBRARY_PATH_ORIG e retira
os caminhos GTK/GI/fontconfig empacotados. Preserva a sessão D-Bus/Wayland.
É o cuidado documentado pelo
[PyInstaller para aplicativos externos](https://pyinstaller.org/en/stable/common-issues-and-pitfalls.html#launching-external-programs-from-the-frozen-application).

Meus saves também oferece importar/exportar sem depender do gerenciador de
arquivos. A importação valida o formato/tamanho, grava uma cópia do cartão
anterior e substitui o cartão ativo atomicamente. Uma falha de disco não apaga
o cartão anterior. A interface impede transferências enquanto seu jogo ou
preparação estão ativos. Os testes usam dados sintéticos e cópias privadas.

`tools/linux/test_desktop_services.py` executa o launcher PyInstaller real,
transfere um cartão entre duas pastas de usuário e inicia um helper do host que
rejeita o ambiente contaminado. A abertura pelo aplicativo padrão real do
notebook também passou, apontando somente para a pasta privada de teste.

## Limite da conclusão

O teste de registradores comprova o defeito e sua correção. A caminhada local
no altar não substitui a reprodução da queda no meio da rampa de Gaius.
A PR continua aberta em draft. Os cartões originais do usuário não são
substituídos pelos testes, e nenhum conteúdo deles é incluído no pacote.
