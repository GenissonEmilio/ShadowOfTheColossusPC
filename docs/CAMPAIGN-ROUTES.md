# Rotas para validar a campanha no Linux

Retomar pelo save normal de QA com nove derrotados, que abriu Dirge no teste
de Basaran. A ordem principal é **Dirge → Celosia → Pelagia → Phalanx →
Cenobia → Argus → Malus e encerramento**. A revanche de Hydrus usa um perfil
de QA separado. [Registro da campanha](CAMPAIGN-VALIDATION.md#basaran--10-de-novembro-de-2026)

As marcas de tempo vêm de quadros amostrados na playlist e indicam onde
rever um trecho do vídeo. Use os marcos do cenário para navegar no port e
confirme nele a resposta dos controles, a colisão e os efeitos em movimento.
[Playlist de referência](https://www.youtube.com/playlist?list=PLrVlvcyP9YsC0Tfre61EzprUSQJ9tl5og)

## Como reduzir o tempo de cada teste

1. Carregar o cartão privado de QA, anotar versão, edição do disco, renderizador
   e escala. Jogar no Normal, com assistências desativadas. Fazer o acesso uma
   vez e gravar o estado na entrada da arena, após a cena de apresentação se
   o jogo estiver carregando.
2. Reservar slots manuais em **F1 → Estados** para a primeira abertura difícil,
   cada mudança decisiva de fase e o último sigilo. Usar **F5/F8** para ajustes
   curtos. Guardar a entrada da arena em slot próprio para que F5 possa ser usado
   durante a luta.
3. Capturar cada sigilo exposto com a espada equipada, antes de atingi-lo.
   Na estocada, verificar círculo de força e dano. Depois
   do último golpe, acompanhar a cena inteira, o retorno e os efeitos que
   podem persistir. Para defeitos visuais, capturar alguns segundos em
   movimento e comparar a mesma fase com o vídeo.
4. Salvar pelo menu original, recarregar o cartão e confirmar o próximo
   colosso. Usar esse cartão ao mudar de build. Estados rápidos dependem
   do mesmo executável, da edição do disco e do cartão correspondente.
   [Compatibilidade dos estados](../README.md#save-states)

## Próximas três lutas

### 10. Dirge

- **Acesso:** do templo, usar a ponte a oeste, cruzar o campo, contornar a
  montanha e passar pela torre até a caverna. Guardar um estado no primeiro
  ponto seguro da arena com Agro. [Entrada da caverna, 6:45](https://www.youtube.com/watch?v=nyu4fVaHSCE&t=405s)
- **Combate:** preparar o arco a cavalo. Durante a perseguição, mirar para
  trás e flechar um olho quando aparecer sobre a areia. Na queda, desmontar,
  subir pela cauda e atacar o sigilo perto da base das costas. Repetir a flecha
  para subir e alcançar o outro sigilo, além do espinho maior.
  [Primeira perseguição, 7:55](https://www.youtube.com/watch?v=nyu4fVaHSCE&t=475s) ·
  [Primeiro embarque, 8:30](https://www.youtube.com/watch?v=nyu4fVaHSCE&t=510s) ·
  [Segundo sigilo, 10:30](https://www.youtube.com/watch?v=nyu4fVaHSCE&t=630s)
- **Pontos de teste:** salvar antes da mira e após a queda, quando Wander
  estiver seguro. Conferir poeira e olhos em movimento, resposta e animação
  de Agro ao mirar, colisão da cauda e brilho dos dois sigilos antes de atacar.

### 11. Celosia

- **Acesso:** seguir para o norte sob o aqueduto, encontrar o arco quebrado
  na borda do desfiladeiro, descer à água e subir até o templo. Guardar a
  passagem antes do salto com agarramento para a sala dos braseiros.
  [Desfiladeiro, 4:00](https://www.youtube.com/watch?v=6UJV7NEdBJM&t=240s) ·
  [Salto de entrada, 5:10](https://www.youtube.com/watch?v=6UJV7NEdBJM&t=310s)
- **Combate:** subir num braseiro, deixar Celosia bater na base, recolher a
  tocha e acendê-la. Aproximar-se devagar até que Celosia recue e caia do
  precipício, quebrando a armadura. Saltar e agarrar as costas expostas. Se
  perder o salto, voltar pelos degraus e usar um pedestal para voltar às costas.
  [Braseiro, 5:55](https://www.youtube.com/watch?v=6UJV7NEdBJM&t=355s) ·
  [Tocha, 6:30](https://www.youtube.com/watch?v=6UJV7NEdBJM&t=390s) ·
  [Queda e sigilo, 7:00](https://www.youtube.com/watch?v=6UJV7NEdBJM&t=420s)
- **Pontos de teste:** salvar com a tocha acesa em posição segura e depois
  da queda. Verificar agarramento na entrada e nos pedestais, fogo e luz da
  tocha, quebra da armadura, colisão das costas e sigilo antes do dano.

### 12. Pelagia

- **Acesso:** seguir pela ponte curva a nordeste até a floresta, contornar
  o desfiladeiro e desmontar na cachoeira. Contornar o pilar agarrado,
  descer ao lago e nadar pelos arcos até a plataforma central.
  [Cachoeira e pilar, 4:00](https://www.youtube.com/watch?v=a2CgrblsVuQ&t=240s) ·
  [Entrada no lago, 4:40](https://www.youtube.com/watch?v=a2CgrblsVuQ&t=280s)
- **Combate:** usar as estruturas como cobertura. Flechar uma presa luminosa
  para Pelagia baixar a cabeça e abrir o salto. Usar as pedras da cabeça
  para guiá-lo a um telhado. Pular no telhado, abrigar-se e, quando ele erguer
  as patas dianteiras, pular no pelo da barriga para atingir o sigilo.
  Numa nova volta, nadar atrás do colosso e subir pelo pelo acima da cauda,
  ou repetir a abertura com arco. [Presa e subida, 6:30](https://www.youtube.com/watch?v=a2CgrblsVuQ&t=390s) ·
  [Telhado e barriga, 7:30](https://www.youtube.com/watch?v=a2CgrblsVuQ&t=450s) ·
  [Raios, 13:40](https://www.youtube.com/watch?v=a2CgrblsVuQ&t=820s)
- **Pontos de teste:** guardar um ponto antes da exposição aos projéteis e
  outro no telhado. Registrar câmera durante o nado submerso e a subida à
  superfície, os raios, o impacto em Wander, a colisão do telhado e o
  agarramento na barriga. Comparar o brilho do sigilo antes do primeiro dano.

## Da segunda metade ao encerramento

| Colosso | Rota e mecânica | Ponto de retomada e verificação |
| --- | --- | --- |
| 13. Phalanx | Floresta → túnel → ruínas no deserto. Flechar as três bolsas por baixo, galopar ao lado e saltar numa asa baixa. Atingir os três sigilos das costas, repetindo a subida após o mergulho na areia. | Guardar a aproximação da asa e a transição após o mergulho. Conferir bolsas, agarramento na asa, rotação do corpo e areia. [Bolsas, 6:00](https://www.youtube.com/watch?v=1YS5aTU9DLk&t=360s) · [Asa, 7:40](https://www.youtube.com/watch?v=1YS5aTU9DLk&t=460s) |
| 14. Cenobia | Cânion → caverna com água → ruínas. Fazer o colosso derrubar a sequência de pilares; usar o muro e os escombros para quebrar a armadura. Subir nas costas após atordoá-lo junto a um pilar caído. | Guardar o início da cadeia, o trecho depois do muro e antes de montar. Conferir colisão em pilares derrubados e restos de armadura. [Pilares, 8:20](https://www.youtube.com/watch?v=3e9FyzUZ1_U&t=500s) · [Desabamento, 14:20](https://www.youtube.com/watch?v=3e9FyzUZ1_U&t=860s) |
| 15. Argus | Fortaleza no deserto → arena. Atrair o pisão numa placa basculante, escalar usando os escombros e alcançar a ponte. Após a destruição da ponte, saltar na cabeça. Ferir o braço para derrubar a arma e alcançar o sigilo na palma. | Guardar antes do pisão e depois da quebra da ponte. Conferir a colisão da placa e da ponte, a queda da arma e a palma no chão. [Placas e subida, 6:40](https://www.youtube.com/watch?v=ptG0SbkRyiQ&t=400s) · [Ponte, 8:40](https://www.youtube.com/watch?v=ptG0SbkRyiQ&t=520s) · [Palma, 11:40](https://www.youtube.com/watch?v=ptG0SbkRyiQ&t=700s) |
| 16. Malus | Abrir o portão ao sul com a espada, atravessar a ponte com Agro e subir ao acesso da arena. Avançar de abrigo em abrigo pelos túneis. Escalar do dorso à mão, passar pelo antebraço e pela outra mão, usar o arco no ombro e subir à cabeça. | Retomar no fim do segundo túnel se houver estado compatível; preservar outro na base e um na mão. Rever projéteis, colisão, agarramento e sigilo. Validar também o trecho jogável do final, as cenas e os créditos. [Abrigos, 10:20](https://www.youtube.com/watch?v=F0YgCa2SCn0&t=620s) · [Escalada, 15:20](https://www.youtube.com/watch?v=F0YgCa2SCn0&t=920s) · [Final, 21:20](https://www.youtube.com/watch?v=F0YgCa2SCn0&t=1280s) |

## Hydrus — rota para repetir a validação

A revanche foi concluída no runtime anterior. A v0.9.3 confirmou a sequência
do choque à margem, com uma recarga do checkpoint após o terceiro espinho,
e registrou o sigilo intacto e a câmera durante o mergulho.
[Evidência da v0.9.3](CAMPAIGN-VALIDATION.md#hydrus-na-v093-linux)

1. No perfil privado, partir de Dried Marsh e seguir até a ponte quebrada e
   a descida em espiral. Na revanche, iniciar pela oração nos restos. Gravar
   um estado antes da eletricidade. [Acesso ao lago, 5:05](https://www.youtube.com/watch?v=VKx5pN7iatk&t=305s)
2. Registrar um choque com perda de vida e prosseguir a partir dessa mesma
   tentativa. Agarrar a cauda quando emergir, avançar nas subidas à superfície
   e permanecer agarrado durante o mergulho. Desativar os três espinhos.
   Na cabeça, capturar o sigilo antes da primeira estocada. Conferir também
   a câmera debaixo d'água e o HUD na transição.
   [Cauda e descargas, 7:10](https://www.youtube.com/watch?v=VKx5pN7iatk&t=430s) ·
   [Sigilo da cabeça, 12:15](https://www.youtube.com/watch?v=VKx5pN7iatk&t=735s)
3. Completar a derrota e o retorno, sair da água, andar em terra firme e girar
   a câmera perto dos pés de Wander. Verificar se os brilhos do relato
   aparecem, registrar o estado após o teste e conferir a recarga.
   [Retorno ao lago, 13:00](https://www.youtube.com/watch?v=VKx5pN7iatk&t=780s)

## Retorno aos colossos anteriores

| Colosso | Trecho para retomar |
| --- | --- |
| 2. Quadratus | Cruzar a ponte norte, descer à praia pela direita e contornar os pilares até o portão. Flechar a sola dianteira quando a pata subir. Aproximar-se pelo lado externo do joelho abaixado, saltar e agarrar o pelo. Usar Triângulo com R1 ao chegar às bordas do ombro. Atravessar as placas do dorso com saltos curtos e conferir os sigilos da cauda e da cabeça. [Praia, 4:40](https://www.youtube.com/watch?v=KFPNEWjCp70&t=280s) · [Montagem, 6:20](https://www.youtube.com/watch?v=KFPNEWjCp70&t=380s) |
| 3. Gaius | Nadar à rampa, caminhar devagar no trecho inclinado e saltar à arena. Atrair o golpe no disco de pedra para quebrar o bracelete. Subir pela espada e testar cabeça e ventre. [Rampa, 4:40](https://www.youtube.com/watch?v=qTxTb7-CqUE&t=280s) · [Subida e sigilos, 11:00](https://www.youtube.com/watch?v=qTxTb7-CqUE&t=660s) |
| 4. Phaedra | Usar os túneis sob os montes funerários. Atrair o colosso a uma entrada e sair por outra para agarrar a parte traseira enquanto ele procura Wander. Conferir a transição para o pescoço e a cabeça. [Túneis, 5:20](https://www.youtube.com/watch?v=BnzIw4xQOq8&t=320s) · [Subida, 8:40](https://www.youtube.com/watch?v=BnzIw4xQOq8&t=520s) |
| 5. Avion | Nadar até uma plataforma baixa, chamar a ave com uma flecha e agarrar a asa na investida. Verificar ambas as asas e a cauda, inclusive quando o corpo vira em voo. [Investida, 7:20](https://www.youtube.com/watch?v=Hw3COkZTAj8&t=440s) |
| 6. Barba | Descer ao templo e atravessar os muros até o abrigo entre os pilares. Quando ele baixar a cabeça, agarrar a barba, subir e atacar as marcas da cabeça e das costas. [Abrigo e subida, 6:00](https://www.youtube.com/watch?v=5iIDRW5w_cI&t=360s) |
| 8. Kuromori | Cruzar a água rasa e subir pelas saliências até o coliseu. Do andar alto, flechar as pernas iluminadas do colosso na parede, descer e atacar os sigilos expostos no ventre. Conferir o gás e a queda. [Coliseu, 7:20](https://www.youtube.com/watch?v=bgX2DMlGejM&t=440s) · [Segunda abertura, 9:00](https://www.youtube.com/watch?v=bgX2DMlGejM&t=540s) |

Os detalhes de acesso e os caminhos alternativos foram cruzados com o guia
da edição PlayStation 2. [Guia de Tom Hayes](https://gamefaqs.gamespot.com/ps2/924364-shadow-of-the-colossus/faqs/41818)
