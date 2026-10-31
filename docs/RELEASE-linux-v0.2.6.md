# Linux v0.2.6 — contribuições revisadas

Esta versão integra duas contribuições de Felipe:

- **#4:** a janela do jogo passa a usar a identificação do launcher no Linux, ajudando o desktop a agrupar ambas. Uma identificação personalizada já definida é preservada.
- **#6, com correção adicional:** anisotropia 2x/4x/8x/16x passa a funcionar no OpenGL, limitada à capacidade da GPU. A correção evita manter anisotropia antiga ao renovar texturas/paletas e retornar ao trilinear. A escolha fica no menu **F1 → Qualidade → Filtro de texturas**. O filtro é opcional e pode aumentar o custo de renderização.

**A tela cheia atual foi mantida.** F11, proporção original/esticar, shaders, controles configuráveis, inversão do mouse e retorno ao launcher pelo F1 continuam presentes. Saves e perfis de controle são preservados.

Das 13 PRs examinadas, as demais ficaram de fora por regressões reproduzidas, conflitos com a implementação atual ou falta de validação dos cenários afetados. A análise individual, os resultados e as limitações estão no [relatório da revisão](https://github.com/GenissonEmilio/ShadowOfTheColossusPC/blob/codex/linux-port/docs/PR-REVIEW-2026-10-31.md).

Validação: 25 testes Python; 42 testes de scheduler/kernel; testes nativos de fidelidade e entrada; 14 transições de sampler com uploads; transferência e persistência de cartões; editor de controles GTK; quatro telas do launcher; proporção, shader e restauração da janela em 1920×1080 e 2560×1080. Os testes de GPU foram complementados por comparação de imagens e medições sintéticas na Intel deste notebook. Isso não equivale a validar todas as batalhas ou todos os controles físicos.

Esta release não declara resolvidos os relatos anteriores de colisão na rampa de Gaius ou todos os efeitos dos colossos. A PR de investigação continua em rascunho.

Baixe o `.tar.gz`, extraia e abra `ShadowOfTheColossusPC`. Linux x86_64, glibc 2.39+ e OpenGL 3.3. O pacote contém launcher, runtime, compilador e fontes abertos correspondentes; o usuário fornece o disco USA SCUS-97472. Não acompanha ROM nem save.
