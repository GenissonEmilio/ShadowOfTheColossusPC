## Launcher com a identidade de Shadow of the Colossus

- Novo banner com Wander e o colosso, baseado na arte enviada pelo usuário.
- Ícone transparente no launcher, janela e atalho do Linux.
- Ilustração proporcional ao redimensionar; texto e botões continuam sendo
  elementos reais da interface. Gráficos, shaders e configuração de controle
  continuam disponíveis.
- A automação verifica as imagens empacotadas e abre as quatro páginas do
  launcher sem precisar de ROM.

## Integridade do runtime

Corrigida uma sobreposição de memória: após onze pilhas de callbacks, a pilha
seguinte podia ocupar o espaço dos serviços RPC/TLS/boot. O alocador agora
preserva esses intervalos, inclusive em pedidos maiores e alinhados.
A regressão falhou na biblioteca anterior e passou na nova. Os dez testes de
interrupções e os 18 testes Python passaram.

**A queda através do altar e da rampa de Gaius continua em investigação.**
A correção de memória não é apresentada como solução confirmada desse relato.
Os problemas de efeitos de derrota e raios de Hydrus também continuam abertos.
A PR de validação permanece aberta.

O pacote inclui o compilador Linux e não contém ROM, BIOS, saves nem código
gerado a partir do disco. Use a edição USA SCUS-97472 já importada ou escolha
seu disco no launcher. Saves e configurações permanecem na pasta de dados.
