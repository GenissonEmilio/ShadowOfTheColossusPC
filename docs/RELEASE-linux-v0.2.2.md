## Linux 0.2.2 — Correção do cache gráfico

Corrige um defeito que podia manter geometria e texturas antigas na tela depois
que o jogo alterava seus dados. **Esta release não declara resolvidos os relatos
de colisão nem os raios de Hydrus.**

### Alterações

- O cache de geometria passa a verificar todos os bytes em até 16 quadros,
  em vez de confiar indefinidamente em amostras fixas.
- Endereços reutilizados após uma pausa na exibição são verificados integralmente.
  Geometria observada mudando passa a ser verificada em cada chamada, inclusive
  quando o mesmo buffer muda duas vezes no mesmo quadro.
- O cache de texturas verifica periodicamente todos os pixels, níveis e paleta;
  após detectar animação, verifica o conteúdo em cada quadro.
- Os caminhos de diagnóstico sem atalhos também usam o conteúdo integral.
- Mantidos o assistente de controles genéricos, as correções anteriores de HUD
  e paletas e o uso padrão das rotinas originais traduzidas de colisão.

### Instalar ou atualizar

1. Baixe **ShadowOfTheColossusPC-0.2.2-linux-x86_64.tar.gz** e extraia a pasta inteira.
2. Abra **ShadowOfTheColossusPC** dentro da nova pasta. O compilador está incluído.
3. Sua biblioteca e seus saves continuam em `~/.local/share/ShadowOfTheColossusPC`
   (ou `XDG_DATA_HOME`). Não é necessário apagar ou importar novamente o disco.

A atualização do runtime pode exigir recompilação na primeira abertura. Depois,
o cache é reutilizado. O pacote não inclui ROM, BIOS ou saves; usa a sua cópia da
edição PS2 USA **SCUS-97472**. Requisitos: Linux x86_64, glibc 2.39+, OpenGL 3.3,
8 GB de RAM e pelo menos 15 GB livres.

### Validação e pendências

O teste de regressão reproduziu o defeito de cache no código anterior e passou
com a correção. Também passaram os 18 testes Python do launcher. O build Linux,
o compilador portátil, as regressões nativas, a persistência do cartão e os
testes de tela cheia/shaders passaram no CI do código da correção.

Com o save recebido de **Cliff Path**, foram repetidos os três trajetos com
caminhada lenta e resolução 2,5×: aproximação da pedra (90 s), duas idas e voltas
pelos degraus (110 s) e saída do altar (85 s). Não houve queda observada nesses
trajetos. Os testes usaram cópias privadas dos cartões, preservando os originais.

**Ainda precisam de confirmação em combate:** atravessamento do chão/colossos,
especialmente na rampa de Gaius; raios de Hydrus; efeitos de derrota, pontos
fracos e indicador da carga de ataque. O defeito de cache é comprovado; sua
relação com cada um desses sintomas ainda não foi demonstrada. A PR continua
aberta para essa validação.

[Resultados detalhados](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/blob/codex/linux-port/docs/FIDELITY-2026-10-28.md) ·
[CI do código da correção](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/actions/runs/40736938873) ·
[PR #2](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/pull/2)
