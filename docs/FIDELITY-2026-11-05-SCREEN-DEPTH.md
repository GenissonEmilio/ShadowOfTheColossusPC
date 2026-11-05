# Profundidade da distorção submersa — em validação

A câmera submersa no lago de Gaius apresentou alternância de brilho na versão
Linux 0.3.1. Na gravação examinada, a flag original de câmera submersa permaneceu
ligada por aproximadamente 18 segundos. A repetição com o mesmo executável e
checkpoint, ocultando somente a camada 19, suprimiu as alternâncias. Os valores
originais da cor e do alpha submersos permaneceram constantes em 128 e 96.
Ocultar a camada é um controle de diagnóstico, não a correção do produto.

O efeito emite vértices XYZF2 com profundidade GS máxima de 24 bits. A conversão
nativa inverte a projeção e usa a profundidade do espaço de visão. Ela limitava
o resultado a 0,11, embora o shader nativo tenha seu plano próximo em 0,1.
Isso deixava o efeito atrás de geometria visível entre 0,1 e 0,11, invertendo a
ordenação esperada do teste GEQUAL original. A conversão passa a limitar as
profundidades anteriores ao plano próximo à sua fronteira exata, 0,1. A
projeção regular e os limites de profundidade distante permanecem iguais.

O teste `test_screen_depth.sh` usa pacotes sintéticos e pixels reais de OpenGL.
Uma superfície verde em w=0,105 confirma que a geometria próxima está visível e
escreve profundidade. Uma cópia da tela com Z máximo, modulada por vermelho,
deve desenhar preto à frente dela. O código antigo deixa o pixel verde; a
correção produz preto. O teste também cobre XYZ2 de 32 bits, inversão regular,
câmera ausente e limites inválidos/distantes. Nenhum dado ou instrução do disco
é incorporado à fixture. O teste integra o CI Linux.

A repetição no jogo com a correção ainda está pendente. Hydrus e Pelagia ainda
não foram validados nesta etapa. Os registros anteriores de campanha continuam
separados do cartão de oito derrotados fornecido para estes testes.

`SOTC_HYDRUS_TRACE=1` habilita diagnósticos limitados de pacotes, distorção,
descargas e paletas de pontos fracos. Eles não mudam o desenho nem o formato dos
estados; ficam desligados por padrão. São evidências de entradas do renderizador,
não uma afirmação de que todos os problemas elétricos ou de brilho foram corrigidos.
