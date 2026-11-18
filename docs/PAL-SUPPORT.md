# Edição PAL

O launcher e o runtime deste port usam a edição USA SCUS-97472. A edição
europeia/australiana SCES-53326 v1.00 tem executável `SCES_533.26` e permanece
pendente até haver uma cópia dessa edição para validação.

A recompilação PAL requer seu próprio perfil, com verificação do executável e
dos módulos `KERNEL`, `MANAGER` e `GAMECORE`, captura ou relocação dos módulos
e revisão dos endereços das intervenções nativas. O teste precisa cobrir boot,
seleção de vídeo e idioma, campanha, memory card e save states. O [port PAL
independente de LightVelox](https://github.com/LightVelox/sotc-vibe-pc) documenta
a edição e o método usado no Windows.
