#include "noesis/NsCore/Noesis.h"
#include "noesis/NsRender/GLFactory.h"
#include "noesis/NsGui/IntegrationAPI.h"
#include "noesis/NsGui/IRenderer.h"
#include "noesis/NsGui/IView.h"
#include "noesis/NsGui/Grid.h"
#include "noesis/NsGui/TextBlock.h"
#include "noesis/NsGui/Button.h"

#include "client/client.h"

static Noesis::IView* _view;
static Noesis::Ptr<Noesis::Grid> _xaml;

void NoesisInit()
{
  Noesis::GUI::Init( nullptr, nullptr, nullptr );

  _xaml = Noesis::GUI::ParseXaml<Noesis::Grid>(R"(
      <Grid xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
      xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
          <Viewbox>
              <StackPanel Margin="50">
                  <Button x:Name="start" Content="Cocaine Diesel" Margin="0,30,0,0"/>
                  <Rectangle Height="5" Margin="-10,200,-10,0">
                      <Rectangle.Fill>
                          <RadialGradientBrush>
                              <GradientStop Offset="0" Color="#40000000"/>
                              <GradientStop Offset="1" Color="#00000000"/>
                          </RadialGradientBrush>
                      </Rectangle.Fill>
                  </Rectangle>
                  <TextBlock x:Name="test" Text="hello world"/>
              </StackPanel>
          </Viewbox>
      </Grid>
  )");

  Noesis::Button* btn = _xaml->FindName<Noesis::Button>( "start" );
  btn->MouseEnter() += []( Noesis::BaseComponent* sender, const Noesis::MouseEventArgs& args )
  {
    Cbuf_AddText( "map carfentanil\n" );
  };

  _view = Noesis::GUI::CreateView( _xaml ).GiveOwnership();
  _view->SetIsPPAAEnabled( true );

  Noesis::Ptr<Noesis::RenderDevice> device = NoesisApp::GLFactory::CreateDevice();
  _view->GetRenderer()->Init( device );
}

void NoesisFrame( int width, int height, int time )
{
  Noesis::TextBlock* text = _xaml->FindName<Noesis::TextBlock>( "test" );
  char buf[128];
  snprintf( buf, sizeof( buf ), "%d", time );
  text->SetText( buf );

  _view->SetSize( width, height );

  Vec2 mousemove = GetMousePos();
  _view->MouseMove( mousemove.x, mousemove.y );

  _view->Update( time / 1000.0f );

  _view->GetRenderer()->UpdateRenderTree();
  _view->GetRenderer()->RenderOffscreen();

  _view->GetRenderer()->Render();
}

static void ReshapeFunc( int width, int height )
{
  _view->SetSize( width, height );
}

void NoesisMouseFunc( int button, int state, int x, int y )
{

}