library ieee;
use ieee.std_logic_1164.all;

entity OR_GATE is
port(
  X: in std_logic;
  Y: in std_logic;
  F2: out std_logic
);
end OR_GATE;

architecture behv of OR_GATE is
begin

  process(X, Y) begin
    F2 <= X or Y;			-- behavior des.
  end process;
end behv;
