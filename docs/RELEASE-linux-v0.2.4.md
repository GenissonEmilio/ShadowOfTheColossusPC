## Saves entre computadores

- **Meus saves** agora tem importação e exportação de cartões PS2/PCSX2 de
  8 MB (`.ps2`, 8.650.752 bytes). O cartão anterior é preservado em
  `player/memcards/backups` antes da importação. Feche o jogo para transferir.
- **Abrir pasta** e **Abrir modelo de shader** iniciam o aplicativo padrão
  com as bibliotecas do sistema restauradas. Isso evita que o GTK empacotado
  seja herdado pelo gerenciador de arquivos de outra distribuição.
- Se não houver um aplicativo padrão funcionando, o erro e o caminho aparecem
  na interface. A importação/exportação continua disponível no próprio launcher.
- Depois de importar, escolha **Carregar jogo** no menu do jogo.

## Preservação dos cálculos do jogo

Corrigido o compartilhamento indevido de registradores VU0 entre contextos
suspensos. Uma thread ou callback podia sobrescrever vetores, acumulador,
quociente e flags que outro cálculo ainda usava. Agora esses valores permanecem
em seu contexto; o gerador aleatório mantém a sequência global.

A regressão falhou na biblioteca 0.2.3 e passa com a correção, incluindo
interrupções aninhadas. O CI também executa os testes de kernel e interrupções,
além de transferência de cartão e abertura de pasta pelo launcher empacotado,
sem precisar de ROM.

**A rampa de Gaius ainda não tem correção confirmada em gameplay.**
A caminhada lenta foi repetida no altar Cliff Path com uma cópia do cartão
fornecido, sem queda observada. Isso não reproduz a rampa dos vídeos, e não
justifica declarar o problema encerrado. A PR permanece aberta em draft.

O pacote atualiza o runtime e recompila o cache local na próxima abertura.
Os saves e as configurações ficam preservados. Não contém ROM, BIOS, cartão
do usuário nem código gerado a partir do disco.
