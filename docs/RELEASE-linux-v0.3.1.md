Correções de colisão e apoio na rampa de Gaius.

- Comparações signed do R5900 passam a usar os 64 bits baixos do registrador, corrigindo uma rejeição indevida de aterrissagem.
- O apoio em pequenas dobras de terreno estático considera o raio do personagem, com a mesma orientação no deslocamento e no deslizamento.
- A geometria, os materiais e as consultas originais de colisão permanecem preservados. Paredes, bordas abertas e saídas condicionais são rejeitadas pelas verificações da correção.
- Launcher, tela cheia, perfis de teclado/mouse/controle, shaders e cartões PS2 continuam disponíveis.

Validação: carregamento normal do cartão em uma cópia isolada, caminhada lenta na subida e descida da rampa de Gaius, degrau superior em corrida normal e repetição com F5/F8. A comparação com o mesmo executável/checkpoint reproduziu o bloqueio com o suporte desligado e aprovou a travessia com a configuração padrão. O CI de compilação, entrada, fidelidade e save states passou.

A validação completa da campanha continua pendente; esta atualização registra os cenários da rampa testados nos relatórios de fidelidade.

Na primeira preparação desta versão, o jogo recompila o cache local. Aguarde o progresso no launcher. Estados rápidos de runtimes anteriores exigem a versão anterior; o cartão normal .ps2 permite continuar entre versões. Os arquivos de save são preservados.

Baixe o .tar.gz, extraia e abra ShadowOfTheColossusPC. Linux x86_64, glibc 2.39+ e OpenGL 3.3. Inclui launcher, runtime, compilador e fontes correspondentes. O usuário fornece o disco USA SCUS-97472; o pacote não inclui ROM nem saves.
